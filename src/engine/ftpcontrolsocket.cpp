#include "FileZilla.h"
#include "ftpcontrolsocket.h"
#include "transfersocket.h"
#include "directorylistingparser.h"
#include "directorycache.h"

#include <wx/file.h>

CFtpControlSocket::CFtpControlSocket(CFileZillaEngine *pEngine) : CControlSocket(pEngine)
{
	m_ReceiveBuffer.Alloc(2000);

	m_pTransferSocket = 0;
}

CFtpControlSocket::~CFtpControlSocket()
{
	delete m_pCurOpData;
}

#define BUFFERSIZE 4096
void CFtpControlSocket::OnReceive(wxSocketEvent &event)
{
	LogMessage(__TFILE__, __LINE__, this, Debug_Verbose, _T("OnReceive()"));

	char *buffer = new char[BUFFERSIZE];
	Read(buffer, BUFFERSIZE);
	if (Error())
	{
		if (LastError() != wxSOCKET_WOULDBLOCK)
		{
			LogMessage(::Error, _("Disconnected from server"));
			DoClose();
		}
		delete [] buffer;
		return;
	}

	int numread = LastCount();
	if (!numread)
	{
		delete [] buffer;
		return;
	}

	for (int i = 0; i < numread; i++)
	{
		if (buffer[i] == '\r' ||
			buffer[i] == '\n' ||
			buffer[i] == 0)
		{
			if (m_ReceiveBuffer == _T(""))
				continue;

            LogMessage(Response, m_ReceiveBuffer);
				
			//Check for multi-line responses
			if (m_ReceiveBuffer.Len() > 3)
			{
				if (m_MultilineResponseCode != _T(""))
				{
					if (m_ReceiveBuffer.Left(4) != m_MultilineResponseCode)
 						m_ReceiveBuffer.Empty();
					else // end of multi-line found
					{
						m_MultilineResponseCode.Clear();
						ParseResponse();
					}
				}
				// start of new multi-line
				else if (m_ReceiveBuffer.GetChar(3) == '-')
				{
					// DDD<SP> is the end of a multi-line response
					m_MultilineResponseCode = m_ReceiveBuffer.Left(3) + _T(" ");
				}
				else
				{
					ParseResponse();
				}
			}
			
			m_ReceiveBuffer.clear();
		}
		else
		{
			//The command may only be 2000 chars long. This ensures that a malicious user can't
			//send extremely large commands to fill the memory of the server
			if (m_ReceiveBuffer.Len()<2000)
				m_ReceiveBuffer += buffer[i];
		}
	}

	delete [] buffer;
}

void CFtpControlSocket::OnConnect(wxSocketEvent &event)
{
	LogMessage(Status, _("Connection established, waiting for welcome message..."));
}

void CFtpControlSocket::ParseResponse()
{
	enum Command commandId = GetCurrentCommandId();
	switch (commandId)
	{
	case cmd_connect:
		Logon();
		break;
	case cmd_list:
		ListParseResponse();
		break;
	case cmd_private:
		ChangeDirParseResponse();
		break;
	case cmd_transfer:
		FileTransferParseResponse();
		break;
	default:
		break;
	}
}

class CLogonOpData : public COpData
{
public:
	CLogonOpData()
	{
		logonSequencePos = 0;
		logonType = 0;

		opId = cmd_connect;
	}

	virtual ~CLogonOpData()
	{
	}

	int logonSequencePos;
	int logonType;
};

bool CFtpControlSocket::Logon()
{
	if (!m_pCurOpData)
		m_pCurOpData = new CLogonOpData;

	CLogonOpData *pData = (CLogonOpData *)(m_pCurOpData);

	const int LO = -2, ER = -1;
	const int NUMLOGIN = 9; // currently supports 9 different login sequences
	int logonseq[NUMLOGIN][20] = {
		// this array stores all of the logon sequences for the various firewalls 
		// in blocks of 3 nums. 1st num is command to send, 2nd num is next point in logon sequence array
		// if 200 series response is rec'd from server as the result of the command, 3rd num is next
		// point in logon sequence if 300 series rec'd
		{0,LO,3, 1,LO,ER}, // no firewall
		{3,6,3,  4,6,ER, 5,9,9, 0,LO,12, 1,LO,ER}, // SITE hostname
		{3,6,3,  4,6,ER, 6,LO,9, 1,LO,ER}, // USER after logon
		{7,3,3,  0,LO,6, 1,LO,ER}, //proxy OPEN
		{3,6,3,  4,6,ER, 0,LO,9, 1,LO,ER}, // Transparent
		{6,LO,3, 1,LO,ER}, // USER remoteID@remotehost
		{8,6,3,  4,6,ER, 0,LO,9, 1,LO,ER}, //USER fireID@remotehost
		{9,ER,3, 1,LO,6, 2,LO,ER}, //USER remoteID@remotehost fireID
		{10,LO,3,11,LO,6,2,LO,ER} // USER remoteID@fireID@remotehost
	};

	int nCommand = 0;
	int code = GetReplyCode();
	if (code != 2 && code != 3)
	{
		DoClose(FZ_REPLY_DISCONNECTED);
		return false;
	}
	if (!pData->opState)
	{
		pData->opState = 1;
		nCommand = logonseq[pData->logonType][0];
	}
	else if (pData->opState == 1)
	{
		pData->logonSequencePos = logonseq[pData->logonType][pData->logonSequencePos + code - 1];

		switch(pData->logonSequencePos)
		{
		case ER: // ER means summat has gone wrong
			DoClose();
			return false;
		case LO: //LO means we are logged on
			pData->opState = 2;
			Send(_T("SYST"));
			return false;
		}

		nCommand = logonseq[pData->logonType][pData->logonSequencePos];
	}
	else if (pData->opState == 2)
	{
		LogMessage(Status, _("Connected"));
		ResetOperation(FZ_REPLY_OK);
		return true;
	}

	switch (nCommand)
	{
	case 0:
		Send(_T("USER ") + m_pCurrentServer->GetUser());
		break;
	case 1:
		Send(_T("PASS ") + m_pCurrentServer->GetPass());
		break;
	default:
		ResetOperation(FZ_REPLY_INTERNALERROR);
		break;
	}

	return false;
}

int CFtpControlSocket::GetReplyCode() const
{
	if (m_ReceiveBuffer == _T(""))
		return 0;

	if (m_ReceiveBuffer[0] < '0' || m_ReceiveBuffer[0] > '9')
		return 0;

	return m_ReceiveBuffer[0] - '0';
}

bool CFtpControlSocket::Send(wxString str)
{
	LogMessage(Command, str);
	str += _T("\r\n");
	wxCharBuffer buffer = wxConvCurrent->cWX2MB(str);
	int len = strlen(buffer);
	return CControlSocket::Send(buffer, len);
}

class CListOpData : public COpData
{
public:
	CListOpData()
	{
		opId = cmd_list;
	}

	virtual ~CListOpData()
	{
	}

	bool bPasv;
	bool bTriedPasv;
	bool bTriedActive;

	int port;
	wxString host;

	CServerPath path;
	wxString subDir;
};

enum listStates
{
	list_init = 0,
	list_port_pasv,
	list_type,
	list_list,
	list_waitfinish,
	list_waitlistpre,
	list_waitlist,
	list_waitsocket
};

int CFtpControlSocket::List(CServerPath path /*=CServerPath()*/, wxString subDir /*=_T("")*/)
{
	LogMessage(Status, _("Retrieving directory listing..."));

	CListOpData *pData = static_cast<CListOpData *>(m_pCurOpData);
	if (pData)
	{
		LogMessage(__TFILE__, __LINE__, this, Debug_Info, _T("deleting nonzero pData"));
		delete pData;
	}
	pData = new CListOpData;
	m_pCurOpData = pData;
			
	pData->bPasv = m_pEngine->GetOptions()->GetOptionVal(OPTION_USEPASV);
	pData->bTriedPasv = pData->bTriedActive = false;
	pData->opState = list_port_pasv;

	pData->path = path;
	pData->subDir = subDir;

	int res = ChangeDir(path, subDir);
	if (res != FZ_REPLY_OK)
		return res;

	return ListSend();
}

int CFtpControlSocket::ListSend()
{
	CListOpData *pData = static_cast<CListOpData *>(m_pCurOpData);

	wxString cmd;
	switch (pData->opState)
	{
	case list_port_pasv:
		m_pTransferSocket = new CTransferSocket(m_pEngine, this, ::list);
		if (pData->bPasv)
		{
			pData->bTriedPasv = true;
			cmd = _T("PASV");
		}
		else
		{
			wxString port = m_pTransferSocket->SetupActiveTransfer();
			if (port == _T(""))
			{
				if (pData->bTriedPasv)
				{
					LogMessage(::Error, _("Failed to create listening socket for active mode transfer, trying passive mode"));
					ResetOperation(FZ_REPLY_ERROR);
					return FZ_REPLY_ERROR;
				}
				pData->bTriedActive = true;
				pData->bTriedPasv = true;
				pData->bPasv = true;
				cmd = _T("PASV");
			}
			else
				cmd = _T("PORT " + port);
		}
		break;
	case list_type:
		cmd = _T("TYPE A");
		break;
	case list_list:
		cmd = _T("LIST");

		if (pData->bPasv)
		{
			if (!m_pTransferSocket->SetupPassiveTransfer(pData->host, pData->port))
			{
				LogMessage(::Error, _("Could not establish connection to server"));
				ResetOperation(FZ_REPLY_ERROR);
				return FZ_REPLY_ERROR;
			}
		}

		m_pTransferSocket->SetActive();

		break;
	case list_waitfinish:
	case list_waitlist:
	case list_waitsocket:
	case list_waitlistpre:
		break;
	default:
		LogMessage(__TFILE__, __LINE__, this, Debug_Warning, _T("invalid opstate"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}
	if (cmd != _T(""))
		if (!Send(cmd))
			return FZ_REPLY_ERROR;

	return FZ_REPLY_WOULDBLOCK;
}

int CFtpControlSocket::ListParseResponse()
{
	if (!m_pCurOpData)
		return FZ_REPLY_ERROR;

	CListOpData *pData = static_cast<CListOpData *>(m_pCurOpData);
	if (pData->opState == list_init)
		return FZ_REPLY_ERROR;

	int code = GetReplyCode();
	bool error = false;
	switch (pData->opState)
	{
	case list_port_pasv:
		if (code != 2 && code != 3)
		{
			if (pData->bTriedPasv)
				if (pData->bTriedActive)
					error = true;
				else
					pData->bPasv = false;
			else
				pData->bPasv = true;
			break;
		}
		if (pData->bPasv)
		{
			int i, j;
			i = m_ReceiveBuffer.Find(_T("("));
			j = m_ReceiveBuffer.Find(_T(")"));
			if (i == -1 || j == -1)
			{
				if (!pData->bTriedActive)
					pData->bPasv = false;
				else
					error = true;
				break;
			}

			wxString temp = m_ReceiveBuffer.Mid(i+1,(j-i)-1);
			i = temp.Find(',', true);
			long number;
			if (i == -1 || !temp.Mid(i + 1).ToLong(&number))
			{
				if (!pData->bTriedActive)
					pData->bPasv = false;
				else
					error = true;
				break;
			}
			pData->port = number; //get ls byte of server socket
			temp = temp.Left(i);
			i = temp.Find(',', true);
			if (i == -1 || !temp.Mid(i + 1).ToLong(&number))
			{
				if (!pData->bTriedActive)
					pData->bPasv = false;
				else
					error = true;
				break;
			}
			pData->port = number; //add ms byte of server socket
			pData->port += 256 * number;
			pData->host = temp.Left(i);
			pData->host.Replace(_T(","), _T("."));
		}
		pData->opState = list_type;
		break;
	case list_type:
		// Don't check error code here, we can live perfectly without it
		pData->opState = list_list;
		break;
	case list_list:
		if (code != 1)
			error = true;
		else
			pData->opState = list_waitfinish;
		break;
	case list_waitlistpre:
		if (code != 1)
			error = true;
		else
			pData->opState = list_waitlist;
		break;
	case list_waitfinish:
		if (code != 2 && code != 3)
			error = true;
		else
			pData->opState = list_waitsocket;
		break;
	case list_waitlist:
		if (code != 2 && code != 3)
			error = true;
		else
		{
			CDirectoryListing *pListing = m_pTransferSocket->m_pDirectoryListingParser->Parse(m_CurrentPath);

			CDirectoryCache cache;
			cache.Store(*pListing, *m_pCurrentServer, pData->path, pData->subDir);
			
			CDirectoryListingNotification *pNotification = new CDirectoryListingNotification(pListing);
			m_pEngine->AddNotification(pNotification);

			ResetOperation(FZ_REPLY_OK);
			return FZ_REPLY_OK;
		}
		break;
	default:
		LogMessage(Debug_Warning, __TFILE__, __LINE__, _T("Unknown op state"));
		error = true;
	}
	if (error)
	{
		ResetOperation(FZ_REPLY_ERROR);
		return FZ_REPLY_ERROR;
	}

	return ListSend();
}

bool CFtpControlSocket::ParsePwdReply(wxString reply)
{
	int pos1 = reply.Find('"');
	int pos2 = reply.Find('"', true);
	if (pos1 == -1 || pos1 >= pos2)
	{
		LogMessage(__TFILE__, __LINE__, this, Debug_Info, _T("No quoted path found in pwd reply, trying first token as path"));
		pos1 = reply.Find(' ');
		if (pos1 == -1)
		{
			LogMessage(__TFILE__, __LINE__, this, Debug_Warning, _T("Can't parse path"));
			return false;
		}

		pos2 = reply.Find(' ', pos1 + 1);
		if (pos2 == -1)
			pos2 = reply.Length();
	}
	reply = reply.Mid(pos1 + 1, pos2 - pos1 - 1);

	m_CurrentPath.SetType(m_pCurrentServer->GetType());
	if (!m_CurrentPath.SetPath(reply))
	{
		LogMessage(__TFILE__, __LINE__, this, Debug_Warning, _T("Can't parse path"));
		return false;
	}

	return true;
}

int CFtpControlSocket::ResetOperation(int nErrorCode)
{
	delete m_pTransferSocket;
	m_pTransferSocket = 0;

	bool next = false;
	if (nErrorCode == FZ_REPLY_OK && m_pCurOpData->pNextOpData)
		next = true;
	else if (nErrorCode == FZ_REPLY_ERROR && m_pCurOpData->pNextOpData && m_pCurOpData->pNextOpData->opId == cmd_transfer)
		next = true;
	
	if (next)
	{
		COpData *pNext = m_pCurOpData->pNextOpData;
		m_pCurOpData->pNextOpData = 0;
		delete m_pCurOpData;
		m_pCurOpData = pNext;
		return SendNextCommand();
	}

	return CControlSocket::ResetOperation(nErrorCode);
}

int CFtpControlSocket::SendNextCommand()
{
	if (!m_pCurOpData)
	{
		LogMessage(Debug_Warning, __TFILE__, __LINE__, _T("SendNextCommand called without active operation"));
		ResetOperation(FZ_REPLY_ERROR);
		return FZ_REPLY_ERROR;
	}

	switch (m_pCurOpData->opId)
	{
	case cmd_list:
		return ListSend();
	case cmd_connect:
		return Logon();
	case cmd_private:
		return ChangeDirSend();
	case cmd_transfer:
		return FileTransferSend();
	default:
		LogMessage(::Debug_Warning, __TFILE__, __LINE__, _T("Unknown opID (%d) in SendNextCommand"), m_pCurOpData->opId);
		ResetOperation(FZ_REPLY_INTERNALERROR);
		break;
	}

	return FZ_REPLY_ERROR;
}

class CChangeDirOpData : public COpData
{
public:
	CChangeDirOpData()
	{
		opId = cmd_private;
	}

	virtual ~CChangeDirOpData()
	{
	}

	CServerPath path;
	wxString subDir;

};

enum cwdStates
{
	cwd_init = 0,
	cwd_pwd,
	cwd_cwd,
	cwd_pwd_cwd,
	cwd_cwd_subdir,
	cwd_pwd_subdir
};

int CFtpControlSocket::ChangeDir(CServerPath path /*=CServerPath()*/, wxString subDir /*=_T("")*/)
{
	enum cwdStates state = cwd_init;

	if (path.IsEmpty())
	{
		if (m_CurrentPath.IsEmpty())
			state = cwd_pwd;
		else
			return FZ_REPLY_OK;
	}
	else
	{
		if (m_CurrentPath != path)
			state = cwd_cwd;
		else
		{
			if (subDir == _T(""))
				return FZ_REPLY_OK;
			else
				state = cwd_cwd_subdir;
		}
	}

	CChangeDirOpData *pData = new CChangeDirOpData;
	pData->pNextOpData = m_pCurOpData;
	pData->opState = state;
	pData->path = path;
	pData->subDir = subDir;

	m_pCurOpData = pData;

	return ChangeDirSend();
}

int CFtpControlSocket::ChangeDirParseResponse()
{
	if (!m_pCurOpData)
	{
		ResetOperation(FZ_REPLY_ERROR);
		return FZ_REPLY_ERROR;
	}
	CChangeDirOpData *pData = static_cast<CChangeDirOpData *>(m_pCurOpData);

	int code = GetReplyCode();
	bool error = false;
	switch (pData->opState)
	{
	case cwd_pwd:
		if (code != 2 && code != 3)
			error = true;
		else if (ParsePwdReply(m_ReceiveBuffer))
		{
			ResetOperation(FZ_REPLY_OK);
			return FZ_REPLY_OK;
		}
		else
			error = true;
		break;
	case cwd_cwd:
		if (code != 2 && code != 3)
			error = true;
		else
			pData->opState = cwd_pwd_cwd;
		break;
	case cwd_pwd_cwd:
		if (code != 2 && code != 3)
			error = true;
		else if (ParsePwdReply(m_ReceiveBuffer))
			if (pData->subDir == _T(""))
			{
				ResetOperation(FZ_REPLY_OK);
				return FZ_REPLY_OK;
			}
			else
				pData->opState = cwd_cwd_subdir;
		else
			error = true;
		break;
	case cwd_cwd_subdir:
		if (code != 2 && code != 3)
			error = true;
		else
			pData->opState = cwd_pwd_subdir;
		break;
	case cwd_pwd_subdir:
		if (code != 2 && code != 3)
			error = true;
		else if (ParsePwdReply(m_ReceiveBuffer))
		{
			ResetOperation(FZ_REPLY_OK);
			return FZ_REPLY_OK;
		}
		else
			error = true;
		break;
	}

	if (error)
	{
		ResetOperation(FZ_REPLY_ERROR);
		return FZ_REPLY_ERROR;
	}

	return ChangeDirSend();
}

int CFtpControlSocket::ChangeDirSend()
{
	if (!m_pCurOpData)
	{
		ResetOperation(FZ_REPLY_ERROR);
		return FZ_REPLY_ERROR;
	}
	CChangeDirOpData *pData = static_cast<CChangeDirOpData *>(m_pCurOpData);

	wxString cmd;
	switch (pData->opState)
	{
	case cwd_pwd:
	case cwd_pwd_cwd:
	case cwd_pwd_subdir:
		cmd = _T("PWD");
		cmd = _T("PWD");
		cmd = _T("PWD");
		break;
	case cwd_cwd:
		cmd = _T("CWD ") + pData->path.GetPath();
		m_CurrentPath.Clear();
		break;
	case cwd_cwd_subdir:
		if (pData->subDir == _T(""))
		{
			ResetOperation(FZ_REPLY_INTERNALERROR);
			return FZ_REPLY_ERROR;
		}
		else if (pData->subDir == _T(".."))
			cmd = _T("CDUP");
		else
			cmd = _T("CWD ") + pData->subDir;
		m_CurrentPath.Clear();
		break;
	}

	if (cmd != _T(""))
		if (!Send(cmd))
			return FZ_REPLY_ERROR;

	return FZ_REPLY_WOULDBLOCK;
}

CFileTransferOpData::CFileTransferOpData()
{
	opId = cmd_transfer;
	pFile = 0;
	resume = false;
	totalSize = leftSize = -1;
	tryAbsolutePath = false;
	bTriedPasv = bTriedActive = false;
}

CFileTransferOpData::~CFileTransferOpData()
{
	delete pFile;
}

enum filetransferStates
{
	filetransfer_init = 0,
	filetransfer_waitcwd,
	filetransfer_waitlist,
	filetransfer_size,
	filetransfer_mdtm,
	filetransfer_type,
	filetransfer_port_pasv,
	filetransfer_rest,
	filetransfer_transfer,
	filetransfer_waitfinish,
	filetransfer_waittransferpre,
	filetransfer_waittransfer,
	filetransfer_waitsocket
};

int CFtpControlSocket::FileTransfer(const wxString localFile, const CServerPath &remotePath, const wxString &remoteFile, bool download)
{
	LogMessage(Status, _("Retrieving directory listing..."));

	if (m_pCurOpData)
	{
		LogMessage(__TFILE__, __LINE__, this, Debug_Info, _T("deleting nonzero pData"));
		delete m_pCurOpData;
	}
	
	CFileTransferOpData *pData = new CFileTransferOpData;
	m_pCurOpData = pData;

	pData->localFile = localFile;
	pData->remotePath = remotePath;
	pData->remoteFile = remoteFile;
	pData->download = download;
			
	pData->bPasv = m_pEngine->GetOptions()->GetOptionVal(OPTION_USEPASV);
	pData->opState = filetransfer_type;

	int res = ChangeDir(remotePath);
	if (res != FZ_REPLY_OK)
		return res;

	return FileTransferSend();
}

int CFtpControlSocket::FileTransferParseResponse()
{
	if (!m_pCurOpData)
		return FZ_REPLY_ERROR;

	CFileTransferOpData *pData = static_cast<CFileTransferOpData *>(m_pCurOpData);
	if (pData->opState == list_init)
		return FZ_REPLY_ERROR;

	int code = GetReplyCode();
	bool error = false;
	switch (pData->opState)
	{
	case filetransfer_type:
		if (code == 2 || code == 3)
			pData->opState = filetransfer_port_pasv;
		else
			error = true;
		break;
	case filetransfer_port_pasv:
		if (code != 2 && code != 3)
		{
			if (pData->bTriedPasv)
				if (pData->bTriedActive)
					error = true;
				else
					pData->bPasv = false;
			else
				pData->bPasv = true;
			break;
		}

		if (pData->bPasv)
		{
			int i, j;
			i = m_ReceiveBuffer.Find(_T("("));
			j = m_ReceiveBuffer.Find(_T(")"));
			if (i == -1 || j == -1)
			{
				if (!pData->bTriedActive)
					pData->bPasv = false;
				else
					error = true;
				break;
			}

			wxString temp = m_ReceiveBuffer.Mid(i+1,(j-i)-1);
			i = temp.Find(',', true);
			long number;
			if (i == -1 || !temp.Mid(i + 1).ToLong(&number))
			{
				if (!pData->bTriedActive)
					pData->bPasv = false;
				else
					error = true;
				break;
			}
			pData->port = number; //get ls byte of server socket
			temp = temp.Left(i);
			i = temp.Find(',', true);
			if (i == -1 || !temp.Mid(i + 1).ToLong(&number))
			{
				if (!pData->bTriedActive)
					pData->bPasv = false;
				else
					error = true;
				break;
			}
			pData->port = number; //add ms byte of server socket
			pData->port += 256 * number;
			pData->host = temp.Left(i);
			pData->host.Replace(_T(","), _T("."));
		}

		if (pData->resume && pData->download)
			pData->opState = filetransfer_rest;
		else
			pData->opState = filetransfer_transfer;

		pData->pFile = new wxFile;
		if (pData->download)
		{
			if (pData->resume)
			{
				if (!pData->pFile->Open(pData->localFile, wxFile::write_append))
				{
					LogMessage(::Error, _("Failed to \"%s\" for appending / writing"), pData->localFile.c_str());
					error = true;
				}
			}
			else
			{
				if (!pData->pFile->Open(pData->localFile, wxFile::write))
				{
					LogMessage(::Error, _("Failed to \"%s\" for writing"), pData->localFile.c_str());
					error = true;
				}
			}

			wxString remotefile = pData->remoteFile;

			CDirectoryListing listing;
			CDirectoryCache cache;
			bool found = cache.Lookup(listing, *m_pCurrentServer, m_CurrentPath);
			if (found)
			{
				for (int i = 0; i < listing.m_entryCount; i++)
				{
					if (listing.m_pEntries[i].name == remotefile)
					{
						wxLongLong size = listing.m_pEntries[i].size;
						if (size >= 0)
							pData->totalSize = size;
					}
				}
			}
			
			pData->leftSize = pData->totalSize;
		}
		else
		{
			if (!pData->pFile->Open(pData->localFile, wxFile::read))
			{
				LogMessage(::Error, _("Failed to \"%s\" for reading"), pData->localFile.c_str());
				error = true;
			}
			
			pData->totalSize = pData->leftSize = pData->pFile->Length();
			if (pData->resume)
			{
				wxString remotefile = pData->remoteFile;

				CDirectoryListing listing;
				CDirectoryCache cache;
				bool found = cache.Lookup(listing, *m_pCurrentServer, m_CurrentPath);
				if (found)
				{
					for (int i = 0; i < listing.m_entryCount; i++)
					{
						wxLongLong size = listing.m_pEntries[i].size;
						if (listing.m_pEntries[i].name == remotefile && size >= 0)
						{
							pData->leftSize -= size;
							break;
						}
					}
					wxFileOffset offset = pData->totalSize - pData->leftSize;
					if (pData->pFile->Seek(offset, wxFromStart) == wxInvalidOffset)
					{
						LogMessage(::Error, _("Could not seek to offset %s withing file"), offset.ToString().c_str());
						error = true;
					}
				}
			}
		}
		break;
	case filetransfer_rest:
		if (code == 3 || code == 2)
		{
			if (pData->pFile->Seek(0, wxFromEnd) == wxInvalidOffset)
			{
				LogMessage(::Error, _("Could not seek to end of file"));
				error = true;
				break;
			}
			pData->opState = filetransfer_transfer;
			pData->leftSize = pData->totalSize - pData->pFile->Length();
		}
		else
			error = true;
		break;
	case filetransfer_transfer:
		if (code != 1)
			error = true;
		else
			pData->opState = filetransfer_waitfinish;
		break;
	case filetransfer_waittransferpre:
		if (code != 1)
			error = true;
		else
			pData->opState = filetransfer_waittransfer;
		break;
	case filetransfer_waitfinish:
		if (code != 2 && code != 3)
			error = true;
		else
			pData->opState = filetransfer_waitsocket;
		break;
	case filetransfer_waittransfer:
		if (code != 2 && code != 3)
			error = true;
		else
		{
			ResetOperation(FZ_REPLY_OK);
			return FZ_REPLY_OK;
		}
		break;
	default:
		LogMessage(Debug_Warning, __TFILE__, __LINE__, _T("Unknown op state"));
		error = true;
		break;
	}

	if (error)
	{
		ResetOperation(FZ_REPLY_ERROR);
		return FZ_REPLY_ERROR;
	}

	return FileTransferSend();
}

int CFtpControlSocket::FileTransferSend()
{
	if (!m_pCurOpData)
	{
		LogMessage(__TFILE__, __LINE__, this, Debug_Info, _T("Empty m_pCurOpData"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	CFileTransferOpData *pData = static_cast<CFileTransferOpData *>(m_pCurOpData);
	if (pData->opState == filetransfer_waitcwd)
	{
		pData->opState = filetransfer_type;
	}
	else if (pData->opState == filetransfer_waitlist)
		pData->opState = filetransfer_type;

	wxString cmd;
	switch (pData->opState)
	{
	case filetransfer_type:
		if (pData->binary)
			cmd = _T("TYPE I");
		else
			cmd = _T("TYPE A");
		break;
	case filetransfer_port_pasv:
		m_pTransferSocket = new CTransferSocket(m_pEngine, this, pData->download ? download : upload);
		if (pData->bPasv)
		{
			pData->bTriedPasv = true;
			cmd = _T("PASV");
		}
		else
		{
			wxString port = m_pTransferSocket->SetupActiveTransfer();
			if (port == _T(""))
			{
				if (pData->bTriedPasv)
				{
					LogMessage(::Error, _("Failed to create listening socket for active mode transfer, trying passive mode"));
					ResetOperation(FZ_REPLY_ERROR);
					return FZ_REPLY_ERROR;
				}
				pData->bTriedActive = true;
				pData->bTriedPasv = true;
				pData->bPasv = true;
				cmd = _T("PASV");
			}
			else
				cmd = _T("PORT " + port);
		}
		break;
	case filetransfer_rest:
		if (!pData->pFile)
		{
			LogMessage(::Debug_Warning, _("Can't sent REST command, can't get local file length since pData->pFile is null"));
			ResetOperation(FZ_REPLY_INTERNALERROR);
			return FZ_REPLY_ERROR;
		}
		cmd = _T("REST ") + pData->pFile->Length().ToString();
		break;
	case filetransfer_transfer:
		cmd = pData->download ? _T("RETR ") : _T("STOR ");
		if (pData->tryAbsolutePath)
			cmd += pData->remotePath.GetPath();
		cmd += pData->remoteFile;

		if (pData->bPasv)
		{
			if (!m_pTransferSocket->SetupPassiveTransfer(pData->host, pData->port))
			{
				LogMessage(::Error, _("Could not establish passive connection to server"));
				ResetOperation(FZ_REPLY_ERROR);
				return FZ_REPLY_ERROR;
			}
		}

		m_pTransferSocket->SetActive();
		break;
	}

	if (cmd != _T(""))
		if (!Send(cmd))
			return FZ_REPLY_ERROR;

	return FZ_REPLY_WOULDBLOCK;
}

void CFtpControlSocket::TransferEnd(int reason)
{
	if (reason)
	{
		ResetOperation(FZ_REPLY_ERROR);
		return;
	}

	if (GetCurrentCommandId() == cmd_list)
	{
		if (!m_pCurOpData)
			return;
			
		CListOpData *pData = static_cast<CListOpData *>(m_pCurOpData);
		if (pData->opState < list_list || pData->opState == list_waitlist || pData->opState == list_waitlistpre)
		{
			LogMessage(Debug_Info, __TFILE__, __LINE__, _T("Call to TransferEnd at unusual time"));
			ResetOperation(FZ_REPLY_ERROR);
			return;
		}

		switch (pData->opState)
		{
		case list_list:
			pData->opState = list_waitlistpre;
			break;
		case list_waitfinish:
			pData->opState = list_waitlist;
			break;
		case list_waitsocket:
			{
				CDirectoryListing *pListing = m_pTransferSocket->m_pDirectoryListingParser->Parse(m_CurrentPath);

				CDirectoryCache cache;
				cache.Store(*pListing, *m_pCurrentServer, pData->path, pData->subDir);
	
				CDirectoryListingNotification *pNotification = new CDirectoryListingNotification(pListing);
				m_pEngine->AddNotification(pNotification);

				ResetOperation(FZ_REPLY_OK);
			}
			break;
		default:
			LogMessage(Debug_Warning, __TFILE__, __LINE__, _T("Unknown op state"));
			ResetOperation(FZ_REPLY_ERROR);
		}
	}
	else if (GetCurrentCommandId() == cmd_transfer)
	{
		if (!m_pCurOpData)
			return;
			
		CFileTransferOpData *pData = static_cast<CFileTransferOpData *>(m_pCurOpData);
		if (pData->opState < filetransfer_transfer || pData->opState == filetransfer_waittransfer || pData->opState == filetransfer_waittransferpre)
		{
			LogMessage(Debug_Info, __TFILE__, __LINE__, _T("Call to TransferEnd at unusual time"));
			ResetOperation(FZ_REPLY_ERROR);
			return;
		}

		switch (pData->opState)
		{
		case filetransfer_transfer:
			pData->opState = filetransfer_waittransferpre;
			break;
		case filetransfer_waitfinish:
			pData->opState = filetransfer_waittransfer;
			break;
		case filetransfer_waitsocket:
			{
				ResetOperation(FZ_REPLY_OK);
			}
			break;
		default:
			LogMessage(Debug_Warning, __TFILE__, __LINE__, _T("Unknown op state"));
			ResetOperation(FZ_REPLY_INTERNALERROR);
		}
	}
}
