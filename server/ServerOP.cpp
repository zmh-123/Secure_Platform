﻿#include "ServerOP.h"
#include "TcpSocket.h"
#include "RequestFactory.h"
#include "RequestCodec.h"
#include "RespondCodec.h"
#include "RespondFactory.h"
#include "RsaCrypto.h"
#include <string>
#include <iostream>
#include <fstream>
#include <jsoncpp/json/json.h>
#include <unistd.h>
#include "Hash.h"
using namespace std;
using namespace Json;

/*
	{
		"Port":9898
	}
*/
ServerOP::ServerOP(string json)
{
	// 解析json文件, 读文件 -> Value
	ifstream ifs(json);
	Reader r;
	Value root;
	r.parse(ifs, root);
	// 将root中的键值对value值取出
	m_port = root["Port"].asInt();
	m_serverID = root["ServerID"].asString();
	// 数据库相关的信息
	m_dbUser = root["UserDB"].asString();
	m_dbPwd = root["PwdDB"].asString();
	m_dbConnStr = root["ConnStrDB"].asString();

	// 实例化一个连接oracle数据的对象
	m_occi.connectDB(m_dbUser, m_dbPwd, m_dbConnStr);

	// 实例化共享内存对象
	// 从配置文件中读 key/pathname
	string shmKey = root["ShmKey"].asString();
	int maxNode = root["ShmMaxNode"].asInt();
	// 客户端存储的秘钥只有一个
	m_shm = new SecKeyShm(shmKey, maxNode);
}


void ServerOP::startServer()
{
	m_server = new TcpServer;
	m_server->setListen(m_port);
	while (1)
	{
		cout << "等待客户端连接..." << endl;
		TcpSocket* tcp = m_server->acceptConn();
		if (tcp == NULL)
		{
			continue;
		}
		cout << "与客户端连接成功..." << endl;
		// 通信
		pthread_t tid;
		// 这个回调可以是类的静态函数, 类的友元函数, 普通的函数
		// 友元的类的朋友, 但是不属于这个类
		// 友元函数可以访问当前类的私有成员
		pthread_create(&tid, NULL, workHard, this);
		m_list.insert(make_pair(tid, tcp));
	}
}

void * ServerOP::working(void * arg)
{
	return nullptr;
}

string ServerOP::seckeyAgree(RequestMsg* reqMsg)
{
	// 0. 对签名进行校验 -> 公钥解密 -> 得到公钥
	// 将收到的公钥数据写入本地磁盘
	ofstream ofs("public.pem");
	ofs << reqMsg->data();
	ofs.close();
	// 创建非对称加密对象
	RespondInfo info;
	RsaCrypto rsa("public.pem", false);

	// 创建哈希对象
	Hash sha(T_SHA1);
	sha.addData(reqMsg->data());
	cout << "1111111111111111" << endl;
	bool bl = rsa.rsaVerify(sha.result(), reqMsg->sign());
	cout << "00000000000000000000" << endl;
	if (bl == false)
	{
		cout << "签名校验失败..." << endl;
		info.status = false;
	}
	else
	{
		cout << "签名校验成功..." << endl;
		// 1. 生成随机字符串
		//   对称加密的秘钥, 使用对称加密算法 aes, 秘钥长度: 16, 24, 32byte
		string key = getRandKey(Len16);
		cout << "生成的随机秘钥: " << key << endl;
		// 2. 通过公钥加密
		cout << "aaaaaaaaaaaaaaaa" << endl;
		string seckey = rsa.rsaPubKeyEncrypt(key);
		cout << "加密之后的秘钥: " << seckey << endl;
		// 3. 初始化回复的数据
		info.clientID = reqMsg->clientid();
		info.data = seckey;
		info.serverID = m_serverID;
		info.status = true;	

		// 将生成的新秘钥写入到数据库中 -> 操作 SECKEYINFO
		NodeSecKeyInfo node;
		strcpy(node.clientID, reqMsg->clientid().data());
		strcpy(node.serverID, reqMsg->serverid().data());
		strcpy(node.seckey, key.data());
		node.seckeyID = m_occi.getKeyID();	// 秘钥的ID
		info.seckeyID = node.seckeyID;
		node.status = 1;
		// 初始化node变量
		bool bl = m_occi.writeSecKey(&node);
		if(bl)
		{
			// 成功
			m_occi.updataKeyID(node.seckeyID + 1);
			// 写共享内存
			m_shm->shmWrite(&node);
		}
		else
		{
			// 失败
			info.status = false;
		}
	}

	// 4. 序列化
	CodecFactory* fac = new RespondFactory(&info);
	Codec* c = fac->createCodec();
	string encMsg = c->encodeMsg();
	// 5. 发送数据
	return encMsg;

}

ServerOP::~ServerOP()
{
	if (m_server)
	{
		delete m_server;
	}
	delete m_shm;
}

// 要求: 字符串中包含: a-z, A-Z, 0-9, 特殊字符
string ServerOP::getRandKey(KeyLen len)
{
	// 设置随机数数种子 => 根据时间
	srand(time(NULL));
	int flag = 0;
	string randStr = string();
	char *cs = "~!@#$%^&*()_+}{|\';[]";
	for (int i = 0; i < len; ++i)
	{
		flag = rand() % 4;	// 4中字符类型
		switch (flag)
		{
		case 0:	// a-z
			randStr.append(1, 'a' + rand() % 26);
			break;
		case 1: // A-Z
			randStr.append(1, 'A' + rand() % 26);
			break;
		case 2: // 0-9
			randStr.append(1, '0' + rand() % 10);
			break;
		case 3: // 特殊字符
			randStr.append(1, cs[rand() % strlen(cs)]);
			break;
		default:
			break;
		}
	}
	return randStr;
}

void* workHard(void * arg)
{
	sleep(1);
	string data = string();
	// 通过参数将传递的this对象转换
	ServerOP* op = (ServerOP*)arg;
	// 从op中将通信的套接字对象取出
	TcpSocket* tcp = op->m_list[pthread_self()];
	// 1. 接收客户端数据 -> 编码
	string msg = tcp->recvMsg();
	// 2. 反序列化 -> 得到原始数据 RequestMsg 类型
	CodecFactory* fac = new RequestFactory(msg);
	Codec* c = fac->createCodec();
	RequestMsg* req = (RequestMsg*)c->decodeMsg();
	// 3. 取出数据
	// 判断客户端是什么请求
	switch (req->cmdtype())
	{
	case 1:
		// 秘钥协商
		data = op->seckeyAgree(req);
		break;
	case 2:
		// 秘钥校验
		break;
	default:
		break;
	}

	// 释放资源
	delete fac;
	delete c;
	// tcp对象如何处理
	tcp->sendMsg(data);
	tcp->disConnect();
	op->m_list.erase(pthread_self());
	delete tcp;

	return NULL;
}
