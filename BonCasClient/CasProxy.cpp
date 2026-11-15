// CasProxy.cpp: CCasProxy クラスのインプリメンテーション
//
//////////////////////////////////////////////////////////////////////


#include "StdAfx.h"
#include "CasProxy.h"
#include <map>
#include <mutex>
#include <utility>


#define WM_BONCASPROXY_EVENT		(WM_USER + 100U)
#define TCP_TIMEOUT					1000UL				// 1秒


namespace {
std::mutex connectBackoffLock;
std::map<std::pair<DWORD, WORD>, DWORD> connectErrorTimes;
}


CCasProxy::CCasProxy(const HWND hProxyHwnd)
	: m_hProxyHwnd(hProxyHwnd)
	, m_dwIP(0x7F000001UL)
	, m_wPort(6900UL)
{

}

CCasProxy::~CCasProxy(void)
{
	// サーバから切断
	m_Socket.Close();
}

const void CCasProxy::Setting(const DWORD dwIP, const WORD wPort)
{
	m_dwIP = dwIP;
	m_wPort = wPort;
}

const BOOL CCasProxy::Connect(void)
{
	const std::pair<DWORD, WORD> endpoint(m_dwIP, m_wPort);
	{
		std::lock_guard<std::mutex> guard(connectBackoffLock);
		const DWORD now = ::GetTickCount();
		for (auto it = connectErrorTimes.begin(); it != connectErrorTimes.end();) {
			if (now - it->second >= TCP_TIMEOUT) it = connectErrorTimes.erase(it);
			else ++it;
			}
		if (connectErrorTimes.count(endpoint)) return FALSE;
		}

	if (m_Socket.Connect(m_dwIP, m_wPort, TCP_TIMEOUT)) {
		std::lock_guard<std::mutex> guard(connectBackoffLock);
		connectErrorTimes.erase(endpoint);
		return TRUE;
		}
	{
		std::lock_guard<std::mutex> guard(connectBackoffLock);
		connectErrorTimes[endpoint] = ::GetTickCount();
		}
	return FALSE;
}

const DWORD CCasProxy::TransmitCommand(const BYTE *pSendData, const DWORD dwSendSize, BYTE *pRecvData)
{
	// 送信データ準備
	BYTE SendBuf[256];
	SendBuf[0] = (BYTE)dwSendSize;
	::CopyMemory(&SendBuf[1], pSendData, dwSendSize);

	try{
		// リクエスト送信
		if(!m_Socket.Send(SendBuf, dwSendSize + 1UL, TCP_TIMEOUT))throw (const DWORD)__LINE__;
	
		// レスポンスヘッダ受信
		if(!m_Socket.Recv(SendBuf, 1UL, TCP_TIMEOUT))throw (const DWORD)__LINE__;

		// レスポンスデータ受信
		if(!m_Socket.Recv(pRecvData, SendBuf[0], TCP_TIMEOUT))throw (const DWORD)__LINE__;
		}
	catch(const DWORD dwLine){
		// 通信エラー発生
		m_Socket.Close();
//		SendProxyEvent(CPEI_DISCONNECTED);
		return 0UL;
		}
		
	return SendBuf[0];
}
/*
void CCasProxy::SendEnterProcessEvent(const HWND hProxyHwnd)
{
	// プロセス開始通知送信
	::SendMessage(hProxyHwnd, WM_BONCASPROXY_EVENT, CPEI_ENTERPROCESS, ::GetCurrentProcessId());
}

void CCasProxy::SendExitProcessEvent(const HWND hProxyHwnd)
{
	// プロセス終了通知送信
	::SendMessage(hProxyHwnd, WM_BONCASPROXY_EVENT, CPEI_EXITPROCESS, ::GetCurrentProcessId());
}

const DWORD CCasProxy::SendProxyEvent(WPARAM wParam) const
{
	// ホストにイベントを送信する
	return SendProxyEvent2(m_hProxyHwnd, wParam, ::GetCurrentProcessId());
}

DWORD CCasProxy::SendProxyEvent2(const HWND hProxyHwnd, WPARAM wParam, LPARAM lParam)
{
	// ホストにイベントを送信する(static)
	return (DWORD)::SendMessage(hProxyHwnd, WM_BONCASPROXY_EVENT, wParam, lParam);
}
*/
