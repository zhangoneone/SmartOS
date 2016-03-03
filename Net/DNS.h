﻿#ifndef _SmartOS_DNS_H_
#define _SmartOS_DNS_H_

#include "Net.h"
#include "Net\ITransport.h"

// DNS协议
class DNS
{
public:
	ISocketHost&	Host;	// 主机

	DNS(ISocketHost& host);
	~DNS();

	IPAddress Query(const String& domain, int msTimeout = 2000);	// 解析

	// 快捷查询。借助主机直接查询多次
	static IPAddress Query(ISocketHost& host, const String& domain, int times = 10, int msTimeout = 2000);

private:
	static uint OnReceive(ITransport* port, Array& bs, void* param, void* param2);
	void Process(Array& bs, const IPEndPoint& server);

	ISocket*	Socket;
	Array*	_Buffer;
};

#endif
