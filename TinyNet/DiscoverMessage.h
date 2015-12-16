﻿#ifndef __DiscoverMessage_H__
#define __DiscoverMessage_H__

#include "Message\MessageBase.h"

// 发现消息
//格式：2设备类型 + N系统ID
class DiscoverMessage : public MessageBase
{
public:
	// 请求数据
	ushort		Type;		// 类型
	ByteArray	HardID;		// 硬件ID。一般16字节
	ushort		Version;	//软件版本
	ushort		PanID;		//无线网段
	byte		SendMode; 	//发送模式	
	byte		Channel;	//通道号 

	// 响应数据
	byte		Address;// 分配得到的设备ID
	ByteArray	Pass;	// 通信密码。一般8字节
	
	// 初始化消息，各字段为0
	DiscoverMessage();

	// 从数据流中读取消息
	virtual bool Read(Stream& ms);
	// 把消息写入数据流中
	virtual void Write(Stream& ms);
	
#if DEBUG
	virtual String& ToStr(String& str) const;
#endif
};

#endif
