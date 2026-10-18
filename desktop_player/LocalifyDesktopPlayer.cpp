#define UNICODE
#define _UNICODE
#define NOMINMAX
#include <windows.h>
#include <winsock2.h>
#include <winhttp.h>
#include <gdiplus.h>
#include <dwmapi.h>
#include <windowsx.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cctype>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#pragma comment(lib,"ws2_32.lib")
#pragma comment(lib,"winhttp.lib")
#pragma comment(lib,"gdiplus.lib")
#pragma comment(lib,"dwmapi.lib")
using namespace Gdiplus;

static const int PORT=4219;
static const char* CLIENT_ID="1550620411740561568";
static const char* DEFAULT_COVER_URL="https://raw.githubusercontent.com/blibbbye/Localify/main/favicon.png";
struct Presence{bool playing=false,shuffle=false;std::string song,artist,album,cover,localCoverData,localCoverKey,repeat="off";double pos=0,dur=0,volume=0.9;uint64_t version=0;};
struct Cmd{uint64_t seq=0;std::string action;double value=0;};
static HWND H=nullptr;static ULONG_PTR GP=0;static std::mutex M;
static Presence P;static Cmd C;static std::unique_ptr<Bitmap> Cover;static std::string CoverUrl;
static uint64_t CoverToken=0;static std::atomic<bool> Run{true},Server{false},Discord{false};static std::atomic<int> DiscordState{0};static std::atomic<bool> DiscordReconnectRequested{false};static HANDLE Pipe=INVALID_HANDLE_VALUE;
static std::string DiscordClientId=CLIENT_ID;
static std::wstring DiscordDiagnostic=L"Connecting to Discord...";
static std::mutex DiscordStatusMutex;

static std::wstring W(const std::string&s){if(s.empty())return L"";int n=MultiByteToWideChar(CP_UTF8,0,s.data(),(int)s.size(),0,0);if(n<=0)return L"";std::wstring r(n,L'\0');MultiByteToWideChar(CP_UTF8,0,s.data(),(int)s.size(),r.data(),n);return r;}
static std::string esc(const std::string&s){std::string r;for(unsigned char c:s){switch(c){case '"':r+="\\\"";break;case '\\':r+="\\\\";break;case '\n':r+="\\n";break;case '\r':r+="\\r";break;default:r.push_back((char)c);}}return r;}
static std::string js(const std::string&j,const char*k){
 std::string q="\"";q+=k;q+="\"";size_t p=j.find(q);if(p==std::string::npos)return"";p=j.find(':',p+q.size());if(p==std::string::npos)return"";++p;while(p<j.size()&&isspace((unsigned char)j[p]))++p;if(p>=j.size()||j[p]!='"')return"";++p;
 std::string r;bool e=false;while(p<j.size()){char c=j[p++];if(e){e=false;if(c=='n')r+='\n';else r+=c;}else if(c=='\\')e=true;else if(c=='"')break;else r+=c;}return r;}
static double jn(const std::string&j,const char*k,double fallback=0){std::string q="\"";q+=k;q+="\"";size_t p=j.find(q);if(p==std::string::npos)return fallback;p=j.find(':',p+q.size());if(p==std::string::npos)return fallback;return atof(j.c_str()+p+1);}

static void setDiscordStatus(int state,const std::string&message){
 DiscordState=state;
 std::lock_guard<std::mutex> lock(DiscordStatusMutex);
 DiscordDiagnostic=W(message);
}
static bool validClientId(const std::wstring&id){
 if(id.size()<17||id.size()>20)return false;
 for(wchar_t ch:id)if(ch<L'0'||ch>L'9')return false;
 return true;
}
static std::wstring discordIniPath(){
 wchar_t module[MAX_PATH]{};
 DWORD n=GetModuleFileNameW(nullptr,module,MAX_PATH);
 std::wstring p(module,n?n:0);
 size_t slash=p.find_last_of(L"\\/");
 if(slash==std::wstring::npos)return L"Localify Discord Player.ini";
 p.resize(slash+1);p+=L"Localify Discord Player.ini";return p;
}
static void loadDiscordClientId(){
 const std::wstring path=discordIniPath();
 if(GetFileAttributesW(path.c_str())==INVALID_FILE_ATTRIBUTES){
  HANDLE f=CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
  if(f!=INVALID_HANDLE_VALUE){
   const char*sample="[Discord]\r\n; If the connector reports an invalid Application ID, replace this with your own ID from Discord Developer Portal > General Information.\r\nClientId=1550620411740561568\r\n";
   DWORD written=0;WriteFile(f,sample,(DWORD)strlen(sample),&written,nullptr);CloseHandle(f);
  }
 }
 wchar_t id[64]{};
 GetPrivateProfileStringW(L"Discord",L"ClientId",W(CLIENT_ID).c_str(),id,64,path.c_str());
 if(!validClientId(id)){
  DiscordClientId.clear();
  setDiscordStatus(4,"Invalid Application ID in Localify Discord Player.ini");
  return;
 }
 int bytes=WideCharToMultiByte(CP_UTF8,0,id,-1,nullptr,0,nullptr,nullptr);
 if(bytes<=1){DiscordClientId.clear();setDiscordStatus(4,"Invalid Application ID in Localify Discord Player.ini");return;}
 std::string value((size_t)bytes,0);
 WideCharToMultiByte(CP_UTF8,0,id,-1,value.data(),bytes,nullptr,nullptr);
 value.resize((size_t)bytes-1);
 DiscordClientId=value;
}
static std::string lowerAscii(std::string s){
 std::transform(s.begin(),s.end(),s.begin(),[](unsigned char c){return (char)std::tolower(c);});
 return s;
}
static int rpcCode(const std::string&payload){
 double code=jn(payload,"code",-1);
 return code>=0&&code<=100000?(int)code:-1;
}
static bool looksLikeInvalidClientId(const std::string&payload,bool isClose){
 const std::string lower=lowerAscii(payload);
 // Discord uses CLOSE code 4000 for an invalid client ID, but ERROR code
 // 4000 means INVALID_PAYLOAD. Do not classify every 4000 response as an ID problem.
 if(isClose&&rpcCode(payload)==4000)return true;
 if(!isClose&&rpcCode(payload)==4007)return true;
 return lower.find("invalid client id")!=std::string::npos ||
        lower.find("invalid client_id")!=std::string::npos ||
        lower.find("invalid client")!=std::string::npos;
}
static std::string describeDiscordResponse(const std::string&payload,const char*fallback){
 const int code=rpcCode(payload);
 const std::string message=js(payload,"message");
 if(code>=0){
  std::string out="Discord RPC error "+std::to_string(code);
  if(!message.empty())out+=": "+message;
  else if(code==4000)out+=": invalid payload (not an Application ID error)";
  return out;
 }
 if(!message.empty())return "Discord: "+message;
 return fallback;
}

static std::string encPath(const std::string&s){
 const char* h="0123456789ABCDEF";std::string r;
 for(unsigned char c:s){if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~')r.push_back((char)c);else{r.push_back('%');r.push_back(h[c>>4]);r.push_back(h[c&15]);}}
 return r;
}
static std::string httpText(const std::string&url){
 HINTERNET se=WinHttpOpen(L"LocalifyDiscordCover/1",WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,0,0,0);if(!se)return"";
 URL_COMPONENTS u{};u.dwStructSize=sizeof(u);wchar_t host[256]{},path[8192]{};u.lpszHostName=host;u.dwHostNameLength=256;u.lpszUrlPath=path;u.dwUrlPathLength=8192;
 std::wstring wu=W(url);HINTERNET co=nullptr,re=nullptr;std::string out;
 if(WinHttpCrackUrl(wu.c_str(),0,0,&u)){co=WinHttpConnect(se,host,u.nPort,0);if(co)re=WinHttpOpenRequest(co,L"GET",path,0,0,0,(u.nScheme==INTERNET_SCHEME_HTTPS)?WINHTTP_FLAG_SECURE:0);}
 if(re&&WinHttpSendRequest(re,0,0,0,0,0,0)&&WinHttpReceiveResponse(re,0)){
  std::vector<char>b;
  for(;;){DWORD n=0;if(!WinHttpQueryDataAvailable(re,&n)||!n)break;if(out.size()+n>2*1024*1024)break;b.resize(n);DWORD got=0;if(!WinHttpReadData(re,b.data(),n,&got)||!got)break;out.append(b.data(),got);}
 }
 if(re)WinHttpCloseHandle(re);if(co)WinHttpCloseHandle(co);WinHttpCloseHandle(se);return out;
}
static std::string jsonStringForKey(const std::string&j,const std::string&key){
 std::string q="\""+esc(key)+"\"";
 size_t p=j.find(q);if(p==std::string::npos)return"";
 p=j.find(':',p+q.size());if(p==std::string::npos)return"";
 ++p;while(p<j.size()&&isspace((unsigned char)j[p]))++p;
 if(p>=j.size()||j[p]!='"')return"";
 ++p;std::string r;bool e=false;
 while(p<j.size()){
  char ch=j[p++];
  if(e){e=false;switch(ch){case 'n':r+='\n';break;case 'r':r+='\r';break;case 't':r+='\t';break;default:r+=ch;break;}}
  else if(ch=='\\')e=true;
  else if(ch=='"')break;
  else r+=ch;
 }
 return r;
}
static std::string registryCoverUrl(const std::string&album){
 static std::mutex lock;static std::string registry;static bool loaded=false;
 std::lock_guard<std::mutex>g(lock);
 if(!loaded){registry=httpText("https://raw.githubusercontent.com/blibbbye/Localify/main/covers.json");loaded=true;}
 if(registry.empty()||album.empty())return"";
 std::string file=jsonStringForKey(registry,album);if(file.empty())return"";
 if(file.rfind("http://",0)==0||file.rfind("https://",0)==0)return file;
 while(!file.empty()&&(file[0]=='.'||file[0]=='/'))file.erase(file.begin());
 return "https://raw.githubusercontent.com/blibbbye/Localify/main/covers/"+encPath(file);
}
static bool jb(const std::string&j,const char*k,bool d){std::string q="\"";q+=k;q+="\"";size_t p=j.find(q);if(p==std::string::npos)return d;p=j.find(':',p+q.size());if(p==std::string::npos)return d;return j.compare(p+1,4,"true")==0;}

static void reply(SOCKET s,const char*st,const char*ct,const std::string&b){
 std::string h="HTTP/1.1 "+std::string(st)+"\r\nContent-Type: "+ct+"\r\nContent-Length: "+std::to_string(b.size())+"\r\nCache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: GET,POST,OPTIONS\r\nAccess-Control-Allow-Headers: Content-Type\r\nConnection: close\r\n\r\n";
 send(s,h.data(),(int)h.size(),0);if(!b.empty())send(s,b.data(),(int)b.size(),0);
}
static std::string req(SOCKET s){
 std::string d;char b[8192];
 size_t headEnd=std::string::npos;
 size_t contentLength=0;
 for(;;){
  int n=recv(s,b,sizeof(b),0);if(n<=0)break;
  d.append(b,n);
  headEnd=d.find("\r\n\r\n");
  if(headEnd!=std::string::npos){
    size_t p=d.find("\r\nContent-Length:");
    if(p==std::string::npos)p=d.find("\r\ncontent-length:");
    if(p!=std::string::npos){
      p=d.find(':',p);
      if(p!=std::string::npos){
        ++p;while(p<d.size()&&(d[p]==' '||d[p]=='\t'))++p;
        contentLength=std::strtoull(d.c_str()+p,nullptr,10);
      }
    }
    const size_t bodyStart=headEnd+4;
    if(d.size()>=bodyStart+contentLength)break;
  }
  if(d.size()>1024*1024)break;
 }
 return d;
}
static std::vector<BYTE> decodeBase64(const std::string&s){
 static const std::string chars="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
 std::vector<BYTE> out;out.reserve(s.size()*3/4);
 uint32_t buffer=0;int bits=-8;
 for(unsigned char ch:s){
  if(ch=='=')break;
  size_t v=chars.find((char)ch);if(v==std::string::npos)continue;
  buffer=(buffer<<6)|(uint32_t)v;bits+=6;
  if(bits>=0){out.push_back((BYTE)((buffer>>bits)&0xFF));bits-=8;}
 }
 return out;
}
static void coverLoad(std::string url,uint64_t token){
 std::thread([url,token]{
  if(url.empty())return;
  std::vector<BYTE> bytes;
  const size_t base64Marker=url.find(";base64,");
  if(url.rfind("data:image/",0)==0&&base64Marker!=std::string::npos){
   bytes=decodeBase64(url.substr(base64Marker+8));
  }else if(url.rfind("https://",0)==0||url.rfind("http://",0)==0){
   HINTERNET se=WinHttpOpen(L"LocalifyDesktopPlayer/4",WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,0,0,0);
   if(se){
    URL_COMPONENTS u{};u.dwStructSize=sizeof(u);wchar_t host[256]{},path[4096]{};
    u.lpszHostName=host;u.dwHostNameLength=256;u.lpszUrlPath=path;u.dwUrlPathLength=4096;
    HINTERNET co=nullptr,re=nullptr;std::wstring wu=W(url);
    if(WinHttpCrackUrl(wu.c_str(),0,0,&u)){
     co=WinHttpConnect(se,host,u.nPort,0);
     if(co)re=WinHttpOpenRequest(se?co:nullptr,L"GET",path,0,0,0,(u.nScheme==INTERNET_SCHEME_HTTPS)?WINHTTP_FLAG_SECURE:0);
    }
    if(re&&WinHttpSendRequest(re,0,0,0,0,0,0)&&WinHttpReceiveResponse(re,0)){
     for(;;){
      DWORD n=0;if(!WinHttpQueryDataAvailable(re,&n)||!n)break;
      if(bytes.size()+n>8*1024*1024){bytes.clear();break;}
      size_t offset=bytes.size();bytes.resize(offset+n);DWORD got=0;
      if(!WinHttpReadData(re,bytes.data()+offset,n,&got)){bytes.resize(offset);break;}
      bytes.resize(offset+got);
     }
    }
    if(re)WinHttpCloseHandle(re);if(co)WinHttpCloseHandle(co);WinHttpCloseHandle(se);
   }
  }
  std::unique_ptr<Bitmap> b;
  if(!bytes.empty()){
   HGLOBAL h=GlobalAlloc(GMEM_MOVEABLE,bytes.size());
   if(h){
    void*p=GlobalLock(h);
    if(p){memcpy(p,bytes.data(),bytes.size());GlobalUnlock(h);}
    IStream*st=nullptr;
    if(SUCCEEDED(CreateStreamOnHGlobal(h,TRUE,&st))){
     Bitmap*x=Bitmap::FromStream(st,FALSE);
     if(x&&x->GetLastStatus()==Ok){
      UINT bw=x->GetWidth(),bh=x->GetHeight();
      std::unique_ptr<Bitmap> copy(new Bitmap(bw,bh,PixelFormat32bppARGB));
      if(copy&&copy->GetLastStatus()==Ok){
       Graphics cg(copy.get());cg.SetInterpolationMode(InterpolationModeHighQualityBicubic);
       cg.DrawImage(x,0,0,(INT)bw,(INT)bh);b=std::move(copy);
      }
     }
     delete x;st->Release();
    }else GlobalFree(h);
   }
  }
  if(b){std::lock_guard<std::mutex>l(M);if(token==CoverToken)Cover=std::move(b);}
  else if(url!=DEFAULT_COVER_URL){coverLoad(DEFAULT_COVER_URL,token);return;}
  InvalidateRect(H,nullptr,FALSE);
 }).detach();
}
static void issue(const char*a,double v=0){std::lock_guard<std::mutex>l(M);++C.seq;C.action=a;C.value=v;}
static void http(SOCKET s){
 std::string r=req(s);size_t e=r.find("\r\n");if(e==std::string::npos){closesocket(s);return;}std::string line=r.substr(0,e);size_t a=line.find(' '),b=line.find(' ',a+1);if(a==std::string::npos||b==std::string::npos){closesocket(s);return;}std::string m=line.substr(0,a),t=line.substr(a+1,b-a-1);
 if(m=="OPTIONS"){reply(s,"204 No Content","text/plain","");closesocket(s);return;}
 if(m=="GET"&&t.rfind("/health",0)==0){std::string body="{\"ok\":true,\"desktopPlayer\":true,\"discordConnected\":"+std::string(Discord.load()?"true":"false")+",\"serverListening\":"+std::string(Server.load()?"true":"false")+",\"port\":4219,\"version\":\"4.5\"}";reply(s,"200 OK","application/json",body);closesocket(s);return;}
 if(m=="GET"&&t.rfind("/control",0)==0){uint64_t q=0;size_t p=t.find("since=");if(p!=std::string::npos)q=_strtoui64(t.c_str()+p+6,0,10);std::lock_guard<std::mutex>l(M);std::string z=C.seq>q?"{\"seq\":"+std::to_string(C.seq)+",\"action\":\""+esc(C.action)+"\",\"value\":"+std::to_string(C.value)+"}":"{\"seq\":"+std::to_string(q)+"}";reply(s,"200 OK","application/json",z);closesocket(s);return;}
 if(m=="POST"&&t=="/presence"){size_t p=r.find("\r\n\r\n");if(p==std::string::npos){reply(s,"400 Bad Request","text/plain","bad");closesocket(s);return;}std::string body=r.substr(p+4);Presence n;n.playing=jb(body,"playing",true);n.shuffle=jb(body,"shuffle",false);n.song=js(body,"song");n.artist=js(body,"artist");n.album=js(body,"album");n.cover=js(body,"coverUrl");n.localCoverData=js(body,"localCoverData");n.localCoverKey=js(body,"localCoverKey");n.repeat=js(body,"repeat");if(n.repeat!="all"&&n.repeat!="one")n.repeat="off";n.pos=std::max(0.0,jn(body,"currentTime"));n.dur=std::max(0.0,jn(body,"duration"));n.volume=std::clamp(jn(body,"volume",0.9),0.0,1.0);
  {std::lock_guard<std::mutex>l(M);
   if(n.localCoverKey.empty()&&n.song==P.song&&n.album==P.album){n.localCoverKey=P.localCoverKey;n.localCoverData=P.localCoverData;}
   else if(n.localCoverData.empty()&&!n.localCoverKey.empty()&&n.localCoverKey==P.localCoverKey)n.localCoverData=P.localCoverData;
  }
  std::string displayCover=n.localCoverData;
  bool localDataCover=displayCover.rfind("data:image/",0)==0&&displayCover.find(";base64,")!=std::string::npos&&displayCover.size()<=11*1024*1024;
  if(!localDataCover)displayCover=n.cover;
  if(!localDataCover&&(displayCover.rfind("https://",0)!=0||displayCover.size()>300))displayCover=registryCoverUrl(n.album);
  if(!localDataCover&&(displayCover.rfind("https://",0)!=0||displayCover.size()>300))displayCover=DEFAULT_COVER_URL;
  bool nc=false;uint64_t tok=0;{
   std::lock_guard<std::mutex>l(M);
   n.version=P.version+1;P=n;
   if(CoverUrl!=displayCover){CoverUrl=displayCover;++CoverToken;tok=CoverToken;nc=true;}
  }
  if(nc){{std::lock_guard<std::mutex>l(M);Cover.reset();}InvalidateRect(H,nullptr,FALSE);coverLoad(displayCover,tok);}
  else InvalidateRect(H,nullptr,FALSE);
  reply(s,"204 No Content","text/plain","");closesocket(s);return;}
 reply(s,"404 Not Found","text/plain","");closesocket(s);
}
static void server(){
 SOCKET l=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);if(l==INVALID_SOCKET)return;sockaddr_in a{};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);a.sin_port=htons(PORT);int yes=1;setsockopt(l,SOL_SOCKET,SO_REUSEADDR,(char*)&yes,sizeof(yes));if(bind(l,(sockaddr*)&a,sizeof(a))==SOCKET_ERROR||listen(l,8)==SOCKET_ERROR){closesocket(l);return;}Server=true;InvalidateRect(H,0,FALSE);while(Run){SOCKET s=accept(l,0,0);if(s!=INVALID_SOCKET)std::thread(http,s).detach();}closesocket(l);
}
static std::string pkt(uint32_t op,const std::string&j){std::string x(8+j.size(),'\0');uint32_t n=(uint32_t)j.size();memcpy(x.data(),&op,4);memcpy(x.data()+4,&n,4);memcpy(x.data()+8,j.data(),j.size());return x;}
static bool pw(uint32_t op,const std::string&j){
 if(Pipe==INVALID_HANDLE_VALUE)return false;
 std::string x=pkt(op,j);DWORD w=0;
 if(!WriteFile(Pipe,x.data(),(DWORD)x.size(),&w,0)||w!=x.size()){
  CloseHandle(Pipe);Pipe=INVALID_HANDLE_VALUE;Discord=false;return false;
 }
 return true;
}
static bool readExactPipe(HANDLE pipe,char*dst,DWORD need,DWORD timeoutMs){
 DWORD have=0;ULONGLONG deadline=GetTickCount64()+timeoutMs;
 while(have<need&&Run&&GetTickCount64()<deadline){
  DWORD available=0;
  if(!PeekNamedPipe(pipe,nullptr,0,nullptr,&available,nullptr))return false;
  if(!available){Sleep(8);continue;}
  DWORD take=std::min<DWORD>(available,need-have),got=0;
  if(!ReadFile(pipe,dst+have,take,&got,nullptr)||!got)return false;
  have+=got;
  // Once a frame has started arriving, allow fragmented named-pipe writes to finish.
  deadline=GetTickCount64()+std::max<DWORD>(timeoutMs,1000);
 }
 return have==need;
}
static bool readRpcPacket(HANDLE pipe,uint32_t&opcode,std::string&payload,DWORD timeoutMs){
 uint32_t header[2]={0,0};
 if(!readExactPipe(pipe,(char*)header,sizeof(header),timeoutMs))return false;
 opcode=header[0];uint32_t length=header[1];
 if(length>2*1024*1024)return false;
 payload.assign(length,'\0');
 return length==0||readExactPipe(pipe,payload.data(),length,timeoutMs);
}
static void disconnectDiscord(){
 if(Pipe!=INVALID_HANDLE_VALUE){CloseHandle(Pipe);Pipe=INVALID_HANDLE_VALUE;}
 Discord=false;
}
static bool connectDiscord(){
 if(Pipe!=INVALID_HANDLE_VALUE&&Discord.load())return true;
 if(Pipe!=INVALID_HANDLE_VALUE)disconnectDiscord();
 if(DiscordClientId.empty()){
  setDiscordStatus(4,"Enter a valid Application ID in Localify Discord Player.ini");
  return false;
 }
 setDiscordStatus(0,"Looking for Discord desktop...");
 bool pipeOpened=false;
 int lastFailure=0;
 std::string lastMessage;
 const std::string hello="{\"v\":1,\"client_id\":\""+DiscordClientId+"\"}";
 for(int i=0;i<10&&Run;i++){
  for(int style=0;style<2&&Run;style++){
   wchar_t name[96];
   if(style==0)swprintf(name,96,L"\\\\?\\pipe\\discord-ipc-%d",i);
   else swprintf(name,96,L"\\\\.\\pipe\\discord-ipc-%d",i);
   HANDLE h=CreateFileW(name,GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);
   if(h==INVALID_HANDLE_VALUE)continue;
   pipeOpened=true;
   Pipe=h;
   setDiscordStatus(0,"Discord IPC found; checking handshake...");
   if(!pw(0,hello)){
    lastFailure=6;lastMessage="Discord closed the IPC connection during handshake";
    disconnectDiscord();continue;
   }
   const ULONGLONG deadline=GetTickCount64()+1800;
   while(Run&&Pipe!=INVALID_HANDLE_VALUE&&GetTickCount64()<deadline){
    uint32_t opcode=0;std::string response;
    DWORD remain=(DWORD)std::max<ULONGLONG>(1,deadline-GetTickCount64());
    if(!readRpcPacket(Pipe,opcode,response,std::min<DWORD>(remain,1800))){
     lastFailure=6;lastMessage="Discord IPC closed or timed out before READY; check the Application ID";
     break;
    }
    if(opcode==3){
     if(!pw(4,response)){lastFailure=6;lastMessage="Could not answer Discord IPC ping";break;}
     continue;
    }
    if(opcode==2){
     OutputDebugStringA(("Localify Discord handshake CLOSE: "+response).c_str());
     if(looksLikeInvalidClientId(response,true)){
      setDiscordStatus(4,"Discord IPC CLOSE 4000: invalid Client ID "+DiscordClientId);
      disconnectDiscord();
      return false;
     }
     const int code=rpcCode(response);
     if(code==4001){lastFailure=5;lastMessage="Discord IPC CLOSE 4001: invalid origin";}
     else if(code==4002){lastFailure=5;lastMessage="Discord IPC CLOSE 4002: rate limited; wait and retry";}
     else if(code==4004){lastFailure=5;lastMessage="Discord IPC CLOSE 4004: invalid RPC version";}
     else if(code==4005){lastFailure=5;lastMessage="Discord IPC CLOSE 4005: invalid encoding";}
     else {lastFailure=5;lastMessage=describeDiscordResponse(response,"Discord closed the IPC handshake; restart Discord and retry");}
     break;
    }
    if(opcode!=1)continue;
    if(response.find("\"evt\":\"READY\"")!=std::string::npos){
     Discord=true;setDiscordStatus(1,"Connected to Discord");
     InvalidateRect(H,nullptr,FALSE);return true;
    }
    if(response.find("\"evt\":\"ERROR\"")!=std::string::npos){
     OutputDebugStringA(("Localify Discord handshake ERROR: "+response).c_str());
     if(looksLikeInvalidClientId(response,false)){
      setDiscordStatus(4,"Discord rejected Client ID "+DiscordClientId+" (RPC error "+std::to_string(rpcCode(response))+")");
      disconnectDiscord();
      return false;
     }
     lastFailure=5;
     lastMessage=describeDiscordResponse(response,"Discord returned an IPC error; restart Discord and retry");
     break;
    }
   }
   if(Pipe!=INVALID_HANDLE_VALUE)disconnectDiscord();
  }
 }
 Discord=false;
 if(lastFailure==0){
  if(pipeOpened){lastFailure=6;lastMessage="Discord pipe opened but no READY reply arrived; check the Application ID";}
  else {lastFailure=2;lastMessage="Discord desktop app was not found; open the Discord desktop app";}
 }
 setDiscordStatus(lastFailure,lastMessage);
 InvalidateRect(H,nullptr,FALSE);
 return false;
}

static bool sendRpcCommand(const std::string&json,const std::string&nonce){
 if(!connectDiscord()||!pw(1,json)){setDiscordStatus(5,"Discord RPC command failed; reconnect the connector");disconnectDiscord();return false;}
 const std::string needle="\"nonce\":\""+nonce+"\"";
 const ULONGLONG deadline=GetTickCount64()+1800;
 while(Run&&GetTickCount64()<deadline){
  uint32_t opcode=0;std::string payload;
  DWORD remain=(DWORD)std::max<ULONGLONG>(1,deadline-GetTickCount64());
  if(!readRpcPacket(Pipe,opcode,payload,std::min<DWORD>(remain,300)))continue;
  if(opcode==3){if(!pw(4,payload)){setDiscordStatus(5,"Could not answer Discord IPC ping");disconnectDiscord();return false;}continue;}
  if(opcode==2){setDiscordStatus(5,"Discord closed the RPC connection");disconnectDiscord();return false;}
  if(opcode==1&&payload.find("\"evt\":\"ERROR\"")!=std::string::npos){OutputDebugStringA(payload.c_str());setDiscordStatus(5,"Discord rejected the RPC activity");disconnectDiscord();return false;}
  if(opcode!=1||payload.find(needle)==std::string::npos)continue;
  return true;
 }
 setDiscordStatus(6,"Discord RPC response timed out");
 return false;
}

static bool activity(const Presence&p){
 if(!connectDiscord())return false;
 std::string discordCover=p.cover;
 if(discordCover.rfind("https://",0)!=0||discordCover.size()>300)discordCover=registryCoverUrl(p.album);
 if(discordCover.rfind("https://",0)!=0||discordCover.size()>300)discordCover=DEFAULT_COVER_URL;
 long long start=(long long)time(nullptr)-(long long)std::max(0.0,p.pos),end=p.dur>0?start+(long long)p.dur:0;
 std::string q="{\"type\":2,\"name\":\""+esc(p.song.empty()?"Unknown song":p.song)+"\",\"details\":\"\",\"state\":\""+esc(p.artist.empty()?"Unknown Artist":p.artist)+"\",\"status_display_type\":1,\"instance\":false";
 q+=",\"assets\":{\"large_image\":\""+esc(discordCover)+"\",\"large_text\":\"Localify artwork\"}";
 if(end>0)q+=",\"timestamps\":{\"start\":"+std::to_string(start*1000)+",\"end\":"+std::to_string(end*1000)+"}";
 q+="}";
 std::string nonce=std::to_string(GetTickCount64());
 std::string z="{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":"+std::to_string(GetCurrentProcessId())+",\"activity\":"+q+"},\"nonce\":\""+nonce+"\"}";
 if(!sendRpcCommand(z,nonce)){disconnectDiscord();return false;}
 return true;
}
static void rpcLoop(){
 uint64_t sent=0;bool active=false;
 while(Run){
  if(DiscordReconnectRequested.exchange(false)){disconnectDiscord();DiscordState=0;sent=0;active=false;}
  Presence p;{std::lock_guard<std::mutex>l(M);p=P;}
  if(p.playing){
   if(p.version!=sent||!Discord.load()){
    if(activity(p)){sent=p.version;active=true;}
    else {sent=0;active=false;}
   }
  }else if(active){
   if(connectDiscord()){
    std::string nonce="clear-"+std::to_string(GetTickCount64());
    std::string z="{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":"+std::to_string(GetCurrentProcessId())+",\"activity\":null},\"nonce\":\""+nonce+"\"}";
    if(!sendRpcCommand(z,nonce))disconnectDiscord();
   }
   active=false;sent=p.version;
  }else connectDiscord();
  Sleep(1000);
 }
}

static void fill(Graphics&g,float x,float y,float w,float h,float r,const Color&c){GraphicsPath p;p.AddArc(x,y,r,r,180,90);p.AddArc(x+w-r,y,r,r,270,90);p.AddArc(x+w-r,y+h-r,r,r,0,90);p.AddArc(x,y+h-r,r,r,90,90);p.CloseFigure();SolidBrush b(c);g.FillPath(&b,&p);}
static void txt(Graphics&g,const wchar_t*s,float x,float y,float w,float h,float size,const Color&c,bool bold=false){FontFamily ff(L"Segoe UI");Font f(&ff,size,bold?FontStyleBold:FontStyleRegular,UnitPixel);SolidBrush b(c);StringFormat sf;sf.SetFormatFlags(StringFormatFlagsNoWrap);sf.SetTrimming(StringTrimmingEllipsisCharacter);g.DrawString(s,-1,&f,RectF(x,y,w,h),&sf,&b);}
static void center(Graphics&g,const wchar_t*s,float x,float y,float w,float h,float size,const Color&c,bool bold=false){FontFamily ff(L"Segoe UI");Font f(&ff,size,bold?FontStyleBold:FontStyleRegular,UnitPixel);SolidBrush b(c);StringFormat sf;sf.SetAlignment(StringAlignmentCenter);sf.SetLineAlignment(StringAlignmentCenter);g.DrawString(s,-1,&f,RectF(x,y,w,h),&sf,&b);}
static RECT closeR(){return{430,8,468,38};}static RECT minR(){return{400,8,430,38};}
static RECT coverRectFor(int w,int h){
 int topPad=18, bottomPad=128;
 int size=std::max(96,std::min(220,std::min(w/3,std::max(96,h-topPad-bottomPad))));
 int y=topPad+20;
 return RECT{22,y,22+size,y+size};
}
static void paint(HDC out){
 RECT rr{};GetClientRect(H,&rr);int w=rr.right,h=rr.bottom;
 if(w<40||h<40)return;
 HDC m=CreateCompatibleDC(out);if(!m)return;
 HBITMAP bm=CreateCompatibleBitmap(out,w,h);if(!bm){DeleteDC(m);return;}
 HGDIOBJ old=SelectObject(m,bm);
 Graphics g(m);g.SetSmoothingMode(SmoothingModeAntiAlias);g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
 g.Clear(Color(255,9,11,15));
 fill(g,8,8,(float)w-16,(float)h-16,18,Color(255,18,21,28));
 SolidBrush glow(Color(18,125,211,252));g.FillEllipse(&glow,-90,-100,340,180);
 txt(g,L"LOCALIFY",22,16,160,24,12,Color(255,125,211,252),true);
 Presence p;{
   std::lock_guard<std::mutex> lock(M);p=P;
 }
 RECT cr=coverRectFor(w,h);int coverW=cr.right-cr.left,coverH=cr.bottom-cr.top;
 fill(g,(float)cr.left,(float)cr.top,(float)coverW,(float)coverH,16,Color(255,30,34,42));
 {
   std::lock_guard<std::mutex> lock(M);
   if(Cover)g.DrawImage(Cover.get(),RectF((REAL)cr.left,(REAL)cr.top,(REAL)coverW,(REAL)coverH));
   else center(g,L"♫",(float)cr.left,(float)cr.top,(float)coverW,(float)coverH,38,Color(255,125,211,252),true);
 }
 float infoX=(float)cr.right+18;
 float infoW=(float)std::max<int>(120,(int)w-cr.right-34);
 std::wstring song=W(p.song.empty()?(p.playing?"Unknown song":"Nothing playing"):p.song);
 std::wstring artist=W(p.artist.empty()?"Unknown Artist":p.artist);
 txt(g,song.c_str(),infoX,(float)cr.top+4,infoW,34,20,Color(255,247,248,250),true);
 txt(g,artist.c_str(),infoX,(float)cr.top+39,infoW,26,12,Color(255,157,165,179));
 txt(g,p.playing?L"PLAYING":L"PAUSED",infoX,(float)cr.top+69,110,18,9,
     p.playing?Color(255,125,211,252):Color(255,145,153,168),true);

 float controlX=infoX;
 float controlW=(float)std::max<int>(150,(int)w-(int)controlX-22);
 int progressY=h-118;
 fill(g,controlX,(float)progressY,controlW,6,3,Color(255,43,48,58));
 double ratio=p.dur>0?std::clamp(p.pos/p.dur,0.0,1.0):0;
 if(ratio>0)fill(g,controlX,(float)progressY,controlW*(float)ratio,6,3,Color(255,125,211,252));
 float by=(float)progressY+13;
 fill(g,controlX,by,50,32,9,Color(255,30,35,44));center(g,L"PREV",controlX,by,50,32,9,Color(255,214,220,228),true);
 fill(g,controlX+58,by,62,32,9,Color(255,125,211,252));center(g,p.playing?L"Ⅱ":L"▶",controlX+58,by,62,32,15,Color(255,7,16,24),true);
 fill(g,controlX+128,by,50,32,9,Color(255,30,35,44));center(g,L"NEXT",controlX+128,by,50,32,9,Color(255,214,220,228),true);

 float optionY=(float)h-59;
 fill(g,controlX,optionY,52,25,7,p.shuffle?Color(255,125,211,252):Color(255,30,35,44));
 center(g,L"SHUFFLE",controlX,optionY,52,25,8,p.shuffle?Color(255,7,16,24):Color(255,193,201,211),true);
 fill(g,controlX+58,optionY,54,25,7,p.repeat!="off"?Color(255,125,211,252):Color(255,30,35,44));
 center(g,p.repeat=="one"?L"REPEAT 1":L"REPEAT",controlX+58,optionY,54,25,8,p.repeat!="off"?Color(255,7,16,24):Color(255,193,201,211),true);
 float volLabelX=controlX+120;
 txt(g,L"VOL",volLabelX,optionY+5,24,15,8,Color(255,157,165,179),true);
 float volX=volLabelX+27,volW=std::max(26.0f,controlX+controlW-volX-3);
 fill(g,volX,optionY+10,volW,5,3,Color(255,43,48,58));
 float volRatio=(float)std::clamp(p.volume,0.0,1.0);
 if(volRatio>0)fill(g,volX,optionY+10,volW*volRatio,5,3,Color(255,125,211,252));
 fill(g,volX+volW*volRatio-4,optionY+5,8,15,4,Color(255,225,243,255));

 txt(g,L"Discord",22,(float)h-30,58,16,9,Server.load()?Color(255,130,240,160):Color(255,145,153,168),true);
 const int ds=DiscordState.load();
 std::wstring discordLabel;
 {std::lock_guard<std::mutex> lock(DiscordStatusMutex);discordLabel=DiscordDiagnostic;}
 if(Discord.load())discordLabel=L"Connected — click to reconnect";
 if(discordLabel.empty())discordLabel=ds==2?L"Open Discord desktop app":L"Connecting to Discord...";
 txt(g,discordLabel.c_str(),(float)80,(float)h-30,(float)std::max(210,w-100),16,9,
     Discord.load()?Color(255,135,230,164):((ds==4||ds==5||ds==6)?Color(255,255,155,120):Color(255,240,200,130)));
 BitBlt(out,0,0,w,h,m,0,0,SRCCOPY);
 SelectObject(m,old);DeleteObject(bm);DeleteDC(m);
}

static LRESULT CALLBACK wnd(HWND h,UINT msg,WPARAM w,LPARAM l){
 switch(msg){
  case WM_ERASEBKGND:return 1;
  case WM_SIZE:InvalidateRect(h,nullptr,FALSE);return 0;
  case WM_PAINT:{
    PAINTSTRUCT ps;HDC dc=BeginPaint(h,&ps);paint(dc);EndPaint(h,&ps);return 0;
  }
  case WM_LBUTTONDOWN:{
    POINT p{GET_X_LPARAM(l),GET_Y_LPARAM(l)};
    RECT rc{};GetClientRect(h,&rc);
    if(p.y>=rc.bottom-34&&p.y<=rc.bottom-8&&p.x>=22&&p.x<=340){DiscordReconnectRequested=true;InvalidateRect(h,nullptr,FALSE);return 0;}
    RECT cr=coverRectFor(rc.right,rc.bottom);
    float infoX=(float)cr.right+18;
    float controlW=(float)std::max<int>(150,rc.right-(int)infoX-22);
    int progressY=rc.bottom-118;
    if(p.y>=progressY-8&&p.y<=progressY+14&&p.x>=(int)infoX&&p.x<=(int)(infoX+controlW)){
      double d;{std::lock_guard<std::mutex> lock(M);d=P.dur;}
      if(d>0)issue("seek",std::clamp((p.x-infoX)/(double)controlW,0.0,1.0)*d);
      return 0;
    }
    float by=(float)progressY+13;
    if(p.y>=(int)by&&p.y<=(int)by+32){
      if(p.x>=(int)infoX&&p.x<(int)infoX+50)issue("prev");
      else if(p.x>=(int)infoX+58&&p.x<(int)infoX+120)issue("toggle");
      else if(p.x>=(int)infoX+128&&p.x<(int)infoX+178)issue("next");
      return 0;
    }
    float optionY=(float)rc.bottom-59;
    if(p.y>=(int)optionY&&p.y<=(int)optionY+25){
      if(p.x>=(int)infoX&&p.x<(int)infoX+52)issue("shuffle");
      else if(p.x>=(int)infoX+58&&p.x<(int)infoX+112)issue("repeat");
      else {
        float volX=infoX+147.0f,volW=std::max(26.0f,infoX+controlW-volX-3);
        if(p.x>=(int)volX&&p.x<=(int)(volX+volW))issue("volume",std::clamp((p.x-volX)/(double)volW,0.0,1.0));
      }
      return 0;
    }
    return 0;
  }
  case WM_TIMER:InvalidateRect(h,nullptr,FALSE);return 0;
  case WM_CLOSE:DestroyWindow(h);return 0;
  case WM_DESTROY:Run=false;PostQuitMessage(0);return 0;
 }
 return DefWindowProcW(h,msg,w,l);
}
int WINAPI wWinMain(HINSTANCE hi,HINSTANCE,LPWSTR,int show){
 GdiplusStartupInput gi;if(GdiplusStartup(&GP,&gi,0)!=Ok)return 1;loadDiscordClientId();WSADATA wd;WSAStartup(MAKEWORD(2,2),&wd);
 WNDCLASSEXW wc{};wc.cbSize=sizeof(wc);wc.hInstance=hi;wc.lpfnWndProc=wnd;wc.lpszClassName=L"LocalifyPlayer3";wc.hCursor=LoadCursor(0,IDC_ARROW);RegisterClassExW(&wc);
 H=CreateWindowExW(WS_EX_APPWINDOW,L"LocalifyPlayer3",L"Localify Discord Player",
  WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,CW_USEDEFAULT,CW_USEDEFAULT,640,390,0,0,hi,0);if(!H)return 1;
 BOOL dark=TRUE;DwmSetWindowAttribute(H,DWMWA_USE_IMMERSIVE_DARK_MODE,&dark,sizeof(dark));
 SetWindowPos(H,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_SHOWWINDOW);
 ShowWindow(H,show);UpdateWindow(H);SetTimer(H,1,500,0);
 std::thread(server).detach();std::thread(rpcLoop).detach();MSG msg;while(GetMessageW(&msg,0,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}Run=false;disconnectDiscord();WSACleanup();GdiplusShutdown(GP);return 0;
}
// Clean rebuild trigger: stable, resizable Windows desktop player 4.4 without asset tooltip text.
