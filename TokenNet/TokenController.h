﻿#ifndef __TokenController_H__
#define __TokenController_H__

#include "Sys.h"
#include "Stream.h"
#include "Net\ITransport.h"

#include "Message\Controller.h"

#include "TokenMessage.h"

// 令牌控制器
class TokenController : public Controller
{
private:
	void*		Server;	// 服务器结点地址

protected:
	virtual bool Dispatch(Stream& ms, Message* pmsg, void* param);
	// 收到消息校验后调用该函数。返回值决定消息是否有效，无效消息不交给处理器处理
	virtual bool Valid(const Message& msg);
	// 接收处理函数
	virtual bool OnReceive(Message& msg);
	virtual bool SendInternal(const Buffer& bs, const void* state);

public:
	uint		Token;	// 令牌
	ByteArray	Key;	// 通信密码

	byte		NoLogCodes[8];	// 没有日志的指令

	TokenController();
	virtual ~TokenController();

	virtual void Open();
	virtual void Close();

	virtual bool Send(Message& msg);
	virtual bool Send(byte code, const Buffer& arr);

	// 响应消息
private:
	void ShowMessage(const char* action, const Message& msg);

	// 统计
private:
	class QueueItem
	{
	public:
		byte	Code;
		UInt64	Time;	// 时间ms
	};

	QueueItem	_Queue[16];

	bool StartSendStat(byte code);
	bool EndSendStat(byte code, bool success);
};

#endif