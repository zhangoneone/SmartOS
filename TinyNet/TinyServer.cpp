﻿#include "Flash.h"
#include "TinyServer.h"
#include "Security\Crc.h"

#include "JoinMessage.h"
#include "PingMessage.h"

#include "Config.h"

#include "Security\MD5.h"

/******************************** TinyServer ********************************/

static bool OnServerReceived(void* sender, Message& msg, void* param);
static void GetDeviceKey(byte scr, Array& key,void* param);

#if DEBUG
// 输出所有设备
static void DeviceShow(void* param);
#endif

TinyServer::TinyServer(TinyController* control)
{
	Control 	= control;
	Cfg			= NULL;
	DeviceType	= Sys.Code;

	Control->Received	= OnServerReceived;
	Control->GetKey		= GetDeviceKey;
	Control->Param		= this;

	Control->Mode		= 2;	// 服务端接收所有消息

	Received	= NULL;
	Param		= NULL;

	Current		= NULL;
	Study		= false;

	Devices.SetLength(0);
}

bool TinyServer::Send(Message& msg) const
{
	return Control->Send(msg);
}

bool TinyServer::Reply(Message& msg) const
{
	return Control->Reply(msg);
}

bool OnServerReceived(void* sender, Message& msg, void* param)
{
	auto server = (TinyServer*)param;
	assert_ptr(server);

	// 消息转发
	return server->OnReceive((TinyMessage&)msg);
}

// 常用系统级消息

void TinyServer::Start()
{
	TS("TinyServer::Start");

	assert_param2(Cfg, "未指定微网服务器的配置");

	//LoadConfig();
	auto count = LoadDevices();
	// 如果加载得到的设备数跟存入的设备数不想等，则需要覆盖一次
	if(Devices.Length() != count) SaveDevices();

	#if DEBUG
	Sys.AddTask(DeviceShow, this, 10000, 30000, "节点列表");
#endif

	Control->Open();
}

// 收到本地无线网消息
bool TinyServer::OnReceive(TinyMessage& msg)
{
	TS("TinyServer::OnReceive");

	// 如果设备列表没有这个设备，那么加进去
	byte id = msg.Src;
	auto dv = Current;
	if(!dv) dv = FindDevice(id);
	// 不响应不在设备列表设备的 非Join指令
	if(!dv && msg.Code > 2) return false;

	switch(msg.Code)
	{
		case 1:
			if(!OnJoin(msg)) return false;
			dv = Current;
			break;
		case 2:
			OnDisjoin(msg);
			break;
		case 3:
			// 设置当前设备
			Current = dv;
			OnPing(msg);
			break;
		case 5:
			// 系统指令不会被转发，这里修改为用户指令
			msg.Code = 0x15;
		case 0x15:
			OnReadReply(msg, *dv);
			break;
		case 6:
		case 0x16:
			//OnWriteReply(msg, *dv);
			break;
	}

	// 更新设备信息
	if(dv) dv->LastTime = Sys.Seconds();

	// 设置当前设备
	Current = dv;

	// 消息转发
	if(Received) return Received(this, msg, Param);

	Current = NULL;

	return true;
}

// 分发外网过来的消息。返回值表示是否有响应
bool TinyServer::Dispatch(TinyMessage& msg)
{
	TS("TinyServer::Dispatch");

	// 非休眠设备直接发送
	//if(!dv->CanSleep())
	//{
		Send(msg);
	//}
	// 休眠设备进入发送队列
	//else
	//{

	//}

	// 先找到设备
	auto dv = FindDevice(msg.Dest);
	if(!dv) return false;

	bool rs = false;

	// 先记好来源地址，避免待会被修改
	byte src	= msg.Src;

	// 缓存内存操作指令
	switch(msg.Code)
	{
		case 5:
		case 0x15:
			rs = OnRead(msg, *dv);
			break;
		case 6:
		case 0x16:
			rs = OnWrite(msg, *dv);
			break;
	}

	// 如果有返回，需要设置目标地址，让网关以为该信息来自设备
	if(rs)
	{
		msg.Dest	= src;
		msg.Src		= dv->Address;
	}

	return rs;
}

// 组网
bool TinyServer::OnJoin(const TinyMessage& msg)
{
	if(msg.Reply) return false;

	TS("TinyServer::OnJoin");

	if(!Study)
	{
		debug_printf("非学习模式禁止加入\r\n");
		return false;
	}

	// 如果设备列表没有这个设备，那么加进去
	byte id = msg.Src;
	if(!id) return false;

	auto now = Sys.Seconds();

	JoinMessage dm;
	dm.ReadMessage(msg);
	if(dm.Kind == 0x1004) return false;

	// 根据硬件编码找设备
	auto dv = FindDevice(dm.HardID);
	if(!dv)
	{
		// 从1开始派ID
		id	= 1;
		while(FindDevice(++id) != NULL && id < 0xFF);
		debug_printf("发现节点设备 0x%04X ，为其分配 0x%02X\r\n", dm.Kind, id);
		if(id == 0xFF) return false;

		dv = new Device();
		dv->Address	= id;
		dv->Logins	= 0;
		// 节点注册
		dv->RegTime	= now;
		dv->Kind	= dm.Kind;
		dv->SetHardID(dm.HardID);
		dv->Version	= dm.Version;
		dv->LoginTime = now;
		// 生成随机密码。当前时间的MD5
		auto bs	= MD5::Hash(Array(&now, 8));
		if(bs.Length() > 8) bs.SetLength(8);
		dv->SetPass(bs);

		if(dv->Valid())
		{
			Devices.Push(dv);
			SaveDevices();	// 写好相关数据 校验通过才能存flash
		}
		else
		{
			delete dv;
			return false;
		}
	}

	// 更新设备信息
	Current		= dv;
	dv->LoginTime	= now;
	dv->Logins++;

	debug_printf("\r\nTinyServer::设备第 %d 次组网 TranID=0x%04X \r\n", dv->Logins, dm.TranID);
	dv->Show(true);

	// 响应
	/*TinyMessage rs;
	rs.Code = msg.Code;
	rs.Dest = msg.Src;
	rs.Seq	= msg.Seq;*/
	auto rs	= msg.CreateReply();

	// 发现响应
	dm.Reply	= true;
	dm.Server	= Cfg->Address;
	dm.Channel	= Cfg->Channel;
	dm.Speed	= Cfg->Speed / 10;
	dm.Address	= dv->Address;
	dm.Password.Copy(dv->GetPass());

	dm.HardID.Set(Sys.ID, 6);
	dm.WriteMessage(rs);

	Reply(rs);

	return true;
}

// 网关重置节点通信密码
bool TinyServer::ResetPassword(byte id) const
{
	TS("TinyServer::ResetPassword");

	ulong nowMs = Sys.Ms();
	auto  nowSec = Sys.Seconds();

	JoinMessage dm;

	// 根据硬件编码找设备
	auto dv = FindDevice(id);
	if(!dv) return false;

	// 生成随机密码。当前时间的MD5
	auto bs	= MD5::Hash(Array(&nowMs, 8));
	if(bs.Length() > 8) bs.SetLength(8);
	//dv->GetPass() = bs;
	dv->SetPass(bs);

	// 响应
	TinyMessage rs;
	rs.Code = 0x01;
	rs.Dest = id;
	//rs.Seq	= id;

	// 发现响应
	dm.Reply	= true;

	dm.Server	= Cfg->Address;
	dm.Channel	= Cfg->Channel;
	dm.Speed	= Cfg->Speed / 10;

	dm.Address	= dv->Address;
	dm.Password.Copy(dv->Pass);

	dm.HardID.Set(Sys.ID, 6);

	dm.WriteMessage(rs);

	Reply(rs);

	return true;
}

// 读取
bool TinyServer::OnDisjoin(const TinyMessage& msg)
{
	TS("TinyServer::OnDisjoin");

	return true;
}

bool TinyServer::Disjoin(TinyMessage& msg, uint crc) const
{
	TS("TinyServer::Disjoin");

	msg.Code = 0x02;
	auto ms = msg.ToStream();
	ms.Write(crc);
	msg.Length = ms.Position();

	Send(msg);

	return true;
}

// 心跳保持与对方的活动状态
bool TinyServer::OnPing(const TinyMessage& msg)
{
	TS("TinyServer::OnPing");

	auto dv = FindDevice(msg.Src);
	// 网关内没有相关节点信息时不鸟他
	if(dv == NULL) return false;

	// 准备一条响应指令
	/*TinyMessage rs;
	rs.Code = msg.Code;
	rs.Dest = msg.Src;
	rs.Seq	= msg.Seq;*/
	auto rs	= msg.CreateReply();

	auto ms	= msg.ToStream();
	PingMessage pm;
	pm.MaxSize	= Control->Port->MaxSize - TinyMessage::MinSize;
	// 子操作码
	while(ms.Remain())
	{
		byte code	= ms.ReadByte();
		switch(code)
		{
			case 0x01:
			{
				auto bs = dv->GetStore();
				pm.ReadData(ms, bs);
				break;
			}
			case 0x02:
			{
				auto bs = dv->GetConfig();
				pm.ReadData(ms, bs);
				break;
			}
			case 0x03:
			{
				ushort crc	= 0;
				if(!pm.ReadHardCrc(ms, dv, crc))
				{
					Disjoin(rs, crc);
					return false;
				}
				break;
			}
			default:
			{
				debug_printf("TinyServer::OnPing 无法识别的心跳子操作码 0x%02X \r\n", code);
				return false;
				//break;
			}
		}
	}
	// 告诉客户端有多少待处理指令

	// 0x02给客户端同步时间，4字节的秒
	auto ms2	= rs.ToStream();
	pm.WriteTime(ms2, Sys.Seconds());
	rs.Length	= ms2.Position();

	Reply(rs);

	return true;
}

/*
请求：1起始 + 1大小
响应：1起始 + N数据
错误：错误码2 + 1起始 + 1大小
*/
bool TinyServer::OnRead(TinyMessage& msg, Device& dv)
{
	if(msg.Reply) return false;
	if(msg.Length < 2) return false;
	if(msg.Error) return false;

	TS("TinyServer::OnRead");

	// 起始地址为7位压缩编码整数
	Stream ms	= msg.ToStream();
	uint offset = ms.ReadEncodeInt();
	uint len	= ms.ReadEncodeInt();

	// 指针归零，准备写入响应数据
	ms.SetPosition(0);

	// 计算还有多少数据可读
	auto bs	= dv.GetStore();
	int remain = bs.Length() - offset;

	while(remain<0)
	{
		debug_printf("读取数据出错Store.Length=%d \r\n", bs.Length()) ;
		offset--;

		remain = bs.Length() - offset;
	}

	if(remain < 0)
	{
		// 可读数据不够时出错
		msg.Error = true;
		ms.Write((byte)2);
		ms.WriteEncodeInt(offset);
		ms.WriteEncodeInt(len);
	}
	else
	{
		ms.WriteEncodeInt(offset);
		// 限制可以读取的大小，不允许越界
		if(len > remain) len = remain;
		if(len > 0) ms.Write(bs.GetBuffer(), offset, len);
	}
	msg.Length	= ms.Position();
	msg.Reply	= true;

	return true;
}

// 读取响应，服务端趁机缓存一份。定时上报也是采用该指令。
bool TinyServer::OnReadReply(const TinyMessage& msg, Device& dv)
{
	if(!msg.Reply || msg.Error) return false;
	if(msg.Length < 2) return false;

	TS("TinyServer::OnReadReply");

	//debug_printf("响应读取写入数据 \r\n") ;
	// 起始地址为7位压缩编码整数
	Stream ms	= msg.ToStream();
	uint offset = ms.ReadEncodeInt();

	auto bs	= dv.GetStore();
	int remain = bs.Capacity() - offset;

	if(remain < 0) return false;

	uint len = ms.Remain();
	if(len > remain) len = remain;
	// 保存一份到缓冲区
	if(len > 0)
	{
		bs.Copy(ms.Current(), len, offset);
	}

	return true;
}

/*
请求：1起始 + N数据
响应：1起始 + 1大小
错误：错误码2 + 1起始 + 1大小
*/
bool TinyServer::OnWrite(TinyMessage& msg, Device& dv)
{
	if(msg.Reply) return false;
	if(msg.Length < 2) return false;

	TS("TinyServer::OnWrite");

	// 起始地址为7位压缩编码整数
	Stream ms	= msg.ToStream();
	uint offset = ms.ReadEncodeInt();

	// 计算还有多少数据可写
	uint len	= ms.Remain();
	auto bs		= dv.GetStore();
	int remain	= bs.Capacity() - offset;
	if(remain < 0)
	{
		msg.Error = true;

		// 指针归零，准备写入响应数据
		ms.SetPosition(0);

		ms.Write((byte)2);
		ms.WriteEncodeInt(offset);
		ms.WriteEncodeInt(len);

		debug_printf("读写指令错误");
	}
	else
	{
		if(len > remain) len = remain;
		// 保存一份到缓冲区
		if(len > 0)
		{
			bs.Copy(ms.Current(), len, offset);

			// 指针归零，准备写入响应数据
			ms.SetPosition(0);
			ms.WriteEncodeInt(offset);
			// 实际写入的长度
			ms.WriteEncodeInt(len);

			//debug_printf("读写指令转换");
		}
	}
	msg.Length	= ms.Position();
	msg.Reply	= true;
	msg.Show();

	return true;
}

Device* TinyServer::FindDevice(byte id) const
{
	if(id == 0) return NULL;

	for(int i=0; i<Devices.Length(); i++)
	{
		if(id == Devices[i]->Address) return Devices[i];
	}

	return NULL;
}

void GetDeviceKey(byte scr,Array& key,void* param)
{
	TS("TinyServer::GetDeviceKey");

	auto server = (TinyServer*)param;

	auto dv = server->FindDevice(scr);
	if(!dv) return;

	// 检查版本
	if(dv->Version < 0x00AA) return;

	// debug_printf("%d 设备获取密匙\n",scr);
	key.Set(dv->Pass, 8);
}

Device* TinyServer::FindDevice(const Array& hardid) const
{
	if(hardid.Length() == 0) return NULL;

	for(int i=0; i<Devices.Length(); i++)
	{
	  if(hardid == Devices[i]->GetHardID()) return Devices[i];
	}
	return NULL;
}

bool TinyServer::DeleteDevice(byte id)
{
	TS("TinyServer::DeleteDevice");

	auto dv = FindDevice(id);
	if(dv && dv->Address == id)
	{
		//Devices.Remove(dv);
		int idx = Devices.FindIndex(dv);
		if(idx >= 0) Devices[idx] = NULL;
		delete dv;
		SaveDevices();

		return true;
	}

	return false;
}

int TinyServer::LoadDevices()
{
	TS("TinyServer::LoadDevices");

	debug_printf("TinyServer::LoadDevices 加载设备！\r\n");
	// 最后4k的位置作为存储位置
	uint addr = 0x8000000 + (Sys.FlashSize << 10) - (4 << 10);
	Flash flash;
	Config cfg(&flash, addr);

	byte* data = (byte*)cfg.Get("Devs");
	if(!data) return 0;

	Stream ms(data, 4 << 10);
	// 设备个数
	int count = ms.ReadByte();
	debug_printf("\t共有%d个节点\r\n", count);
	int i = 0;
	for(; i<count; i++)
	{
		debug_printf("\t加载设备:");

		bool fs	= false;
		/*ms.Seek(1);
		byte id = ms.Peek();
		ms.Seek(-1);*/
		// 前面一个字节是长度，第二个字节才是ID
		byte id = ms.GetBuffer()[1];
		auto dv = FindDevice(id);
		if(!dv)
		{
			dv	= new Device();
			fs	= true;
		}
		dv->Read(ms);
		dv->Show();

		if(fs)
		{
			int idx = Devices.FindIndex(NULL);
			if(idx == -1)
			{
				if(dv->Valid())
					Devices.Push(dv);
				else
					delete dv;
				debug_printf("\t Push");
			}
		}
		debug_printf("\r\n");
	}

	debug_printf("TinyServer::LoadDevices 从 0x%08X 加载 %d 个设备！\r\n", addr, i);

	byte len = Devices.Length();
	debug_printf("Devices内已有节点 %d 个\r\n", len);

	return i;
}

void TinyServer::SaveDevices() const
{
	TS("TinyServer::SaveDevices");

	// 最后4k的位置作为存储位置
	uint addr = 0x8000000 + (Sys.FlashSize << 10) - (4 << 10);
	Flash flash;
	Config cfg(&flash, addr);

	byte buf[0x800];

	MemoryStream ms(buf, ArrayLength(buf));
	// 设备个数
	int count = Devices.Length();
	ms.Write((byte)count);
	for(int i = 0; i<count; i++)
	{
		Device* dv = Devices[i];
		dv->Write(ms);
	}
	debug_printf("TinyServer::SaveDevices 保存 %d 个设备到 0x%08X！\r\n", count, addr);
	cfg.Set("Devs", Array(ms.GetBuffer(), ms.Position()));
}

void TinyServer::ClearDevices()
{
	TS("TinyServer::ClearDevices");

	// 最后4k的位置作为存储位置
	uint addr = 0x8000000 + (Sys.FlashSize << 10) - (4 << 10);
	Flash flash;
	Config cfg(&flash, addr);

	debug_printf("TinyServer::ClearDevices 清空设备列表 0x%08X \r\n", addr);

	cfg.Invalid("Devs");

	int count = Devices.Length();
	for(int j = 0; j < 3; j++)		// 3遍
	{
		for(int i = 1; i < count; i++)	// 从1开始派ID  自己下线完全不需要
		{
			auto dv = Devices[i];
			TinyMessage rs;
			rs.Dest = dv->Address;
			ushort crc = Crc::Hash16(dv->GetHardID());
			Disjoin(rs, crc);
		}
	}

	for(int i=0; i<Devices.Length(); i++)
	{
		delete Devices[i];
	}
	Devices.SetLength(0);	// 清零后需要保存一下，否则重启后 Length 可能不是 0。做到以防万一
	SaveDevices();
}

bool TinyServer::LoadConfig()
{
	TS("TinyServer::LoadConfig");

	debug_printf("TinyServer::LoadDevices 加载设备！\r\n");
	// 最后4k的位置作为存储位置
	uint addr = 0x8000000 + (Sys.FlashSize << 10) - (4 << 10);
	Flash flash;
	Config cfg(&flash, addr);

	byte* data = (byte*)cfg.Get("TCfg");
	if(!data)
	{
		SaveConfig();	// 没有就保存默认配置
		return true;
	}

	Stream ms(data, sizeof(TinyConfig));
	// 读取配置信息
	Cfg->Read(ms);

	debug_printf("TinyServer::LoadConfig 从 0x%08X 加载成功 ！\r\n", addr);

	return true;
}

void TinyServer::SaveConfig() const
{
	TS("TinyServer::SaveConfig");

	// 最后4k的位置作为存储位置
	uint addr = 0x8000000 + (Sys.FlashSize << 10) - (4 << 10);
	Flash flash;
	Config cfg(&flash, addr);

	byte buf[sizeof(TinyConfig)];

	Stream ms(buf, ArrayLength(buf));
	Cfg->Write(ms);

	debug_printf("TinyServer::SaveConfig 保存到 0x%08X！\r\n", addr);

	cfg.Set("TCfg", Array(ms.GetBuffer(), ms.Position()));
}

void TinyServer::ClearConfig()
{
	TS("TinyServer::ClearConfig");

	debug_printf("TinyServer::ClearDevices 设备区清零！\r\n");
	// 最后4k的位置作为存储位置
	uint addr = 0x8000000 + (Sys.FlashSize << 10) - (4 << 10);
	Flash flash;
	Config cfg(&flash, addr);

	debug_printf("TinyServer::ClearConfig 重置配置信息 0x%08X \r\n", addr);

	cfg.Invalid("TCfg");

	for(int i=0; i<Devices.Length(); i++)
		delete Devices[i];
	Devices.SetLength(0);
}

// 输出所有设备
void DeviceShow(void* param)
{
	TS("TinyServer::DeviceShow");

	auto svr	= (TinyServer*)param;

	byte len = svr->Devices.Length();
	debug_printf("\r\n已有节点 %d 个\r\n", len);
	for(int i = 0; i < len; i++)
	{
		auto dv	= svr->Devices[i];
		dv->Show();
		if(i&0x01)
			debug_printf("\r\n");
		else
			debug_printf("\t");

		Sys.Sleep(0);
	}
	debug_printf("\r\n\r\n");
}
