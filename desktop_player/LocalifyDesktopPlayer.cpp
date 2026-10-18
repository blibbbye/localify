#define UNICODE
#define _UNICODE
#define NOMINMAX
#include <windows.h>
#include <winsock2.h>
#include <winhttp.h>
#include <gdiplus.h>
#include <dwmapi.h>
#include <windowsx.h>
#include <shellapi.h>
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
static const char* CLIENT_ID="";
static const wchar_t* LEGACY_UNVERIFIED_CLIENT_ID=L"1550620411740561568";
static const char* DEFAULT_COVER_URL="https://raw.githubusercontent.com/blibbbye/Localify/main/favicon.png";
struct Presence{bool playing=false,shuffle=false;std::string song,artist,album,cover,localCoverData,localCoverKey,repeat="off";double pos=0,dur=0,volume=0.9;uint64_t version=0;};
struct Cmd{uint64_t seq=0;std::string action;double value=0;};
static HWND H=nullptr;static ULONG_PTR GP=0;static std::mutex M;
static Presence P;static Cmd C;static std::unique_ptr<Bitmap> Cover;static std::string CoverUrl;
static uint64_t CoverToken=0;static std::atomic<bool> Run{true},Server{false},Discord{false};static std::atomic<int> DiscordState{0};static std::atomic<bool> DiscordReconnectRequested{false};static HANDLE Pipe=INVALID_HANDLE_VALUE;
static std::string DiscordClientId=CLIENT_ID;
static std::wstring DiscordDiagnostic=L"Connecting to Discord...";
static std::mutex DiscordStatusMutex;
static std::atomic<int> BridgeError{0};
static std::string LastIpcPipe="not connected";
static DWORD LastPipeError=0;
static HWND SettingsWindow=nullptr, ClientIdEdit=nullptr, KeepOnTopCheck=nullptr;
static bool KeepOnTop=false, DragSeeking=false, DragVolume=false;
static const int ID_SETTINGS_SAVE=2101, ID_SETTINGS_DEFAULT=2102, ID_SETTINGS_CANCEL=2103, ID_SETTINGS_OPEN=2104, ID_SETTINGS_COPY=2105, ID_SETTINGS_TOPMOST=2106;

static std::wstring W(const std::string&s){if(s.empty())return L"";int n=MultiByteToWideChar(CP_UTF8,0,s.data(),(int)s.size(),0,0);if(n<=0)return L"";std::wstring r(n,L'\0');MultiByteToWideChar(CP_UTF8,0,s.data(),(int)s.size(),r.data(),n);return r;}
static std::string UTF8(const std::wstring&s){if(s.empty())return"";int n=WideCharToMultiByte(CP_UTF8,0,s.data(),(int)s.size(),nullptr,0,nullptr,nullptr);if(n<=0)return"";std::string r(n,'\0');WideCharToMultiByte(CP_UTF8,0,s.data(),(int)s.size(),r.data(),n,nullptr,nullptr);return r;}
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
   const char*sample="[Discord]\r\n; Required: paste a real Application ID from Discord Developer Portal > General Information.\r\nClientId=\r\n";
   DWORD written=0;WriteFile(f,sample,(DWORD)strlen(sample),&written,nullptr);CloseHandle(f);
  }
 }
 wchar_t id[64]{};
 GetPrivateProfileStringW(L"Discord",L"ClientId",W(CLIENT_ID).c_str(),id,64,path.c_str());
 std::wstring loadedId(id);
 auto ws=[](wchar_t c){return c==L' '||c==L'\t'||c==L'\r'||c==L'\n';};
 while(!loadedId.empty()&&ws(loadedId.front()))loadedId.erase(loadedId.begin());
 while(!loadedId.empty()&&ws(loadedId.back()))loadedId.pop_back();
 // Older builds used an unverified fallback ID. Ask for a real app ID instead of retrying it forever.
 if(loadedId==LEGACY_UNVERIFIED_CLIENT_ID){loadedId.clear();WritePrivateProfileStringW(L"Discord",L"ClientId",L"",path.c_str());}
 if(!validClientId(loadedId)){
  DiscordClientId.clear();
  setDiscordStatus(4,"Setup needed: open Settings and enter your Discord Application ID.");
  return;
 }
 int bytes=WideCharToMultiByte(CP_UTF8,0,loadedId.c_str(),-1,nullptr,0,nullptr,nullptr);
 if(bytes<=1){DiscordClientId.clear();setDiscordStatus(4,"Setup needed: open Settings and enter your Discord Application ID.");return;}
 std::string value((size_t)bytes,0);
 WideCharToMultiByte(CP_UTF8,0,loadedId.c_str(),-1,value.data(),bytes,nullptr,nullptr);
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
 if(!loaded||registry.empty()){registry=httpText("https://raw.githubusercontent.com/blibbbye/Localify/main/covers.json");loaded=!registry.empty();}
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
 if(m=="GET"&&t.rfind("/health",0)==0){std::wstring diagnostic;{std::lock_guard<std::mutex>lock(DiscordStatusMutex);diagnostic=DiscordDiagnostic;}std::string body="{\"ok\":true,\"desktopPlayer\":true,\"discordConnected":"+std::string(Discord.load()?"true":"false")+",\"serverListening\":"+std::string(Server.load()?"true":"false")+",\"discordState\":"+std::to_string(DiscordState.load())+",\"discordStatus\":\""+esc(UTF8(diagnostic))+"\",\"port\":4219,\"version\":\"5.1\"}";reply(s,"200 OK","application/json",body);closesocket(s);return;}
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
 SOCKET l=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
 if(l==INVALID_SOCKET){BridgeError=WSAGetLastError();InvalidateRect(H,nullptr,FALSE);return;}
 sockaddr_in a{};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);a.sin_port=htons(PORT);
 int yes=1;setsockopt(l,SOL_SOCKET,SO_REUSEADDR,(char*)&yes,sizeof(yes));
 if(bind(l,(sockaddr*)&a,sizeof(a))==SOCKET_ERROR||listen(l,8)==SOCKET_ERROR){
  BridgeError=WSAGetLastError();closesocket(l);InvalidateRect(H,nullptr,FALSE);return;
 }
 BridgeError=0;Server=true;InvalidateRect(H,0,FALSE);
 while(Run){SOCKET s=accept(l,0,0);if(s!=INVALID_SOCKET)std::thread(http,s).detach();}
 closesocket(l);
}
static std::string pkt(uint32_t op,const std::string&j){std::string x(8+j.size(),'\0');uint32_t n=(uint32_t)j.size();memcpy(x.data(),&op,4);memcpy(x.data()+4,&n,4);memcpy(x.data()+8,j.data(),j.size());return x;}
static bool pw(uint32_t op,const std::string&j){
 if(Pipe==INVALID_HANDLE_VALUE)return false;
 const std::string x=pkt(op,j);
 size_t sent=0;
 while(sent<x.size()){
  DWORD w=0;
  if(!WriteFile(Pipe,x.data()+sent,(DWORD)(x.size()-sent),&w,nullptr)||!w){
   LastPipeError=GetLastError();
   if(Pipe!=INVALID_HANDLE_VALUE){CloseHandle(Pipe);Pipe=INVALID_HANDLE_VALUE;}
   Discord=false;return false;
  }
  sent+=w;
 }
 return true;
}
static bool readExactPipe(HANDLE pipe,char*dst,DWORD need,DWORD timeoutMs){
 DWORD have=0;ULONGLONG deadline=GetTickCount64()+timeoutMs;
 while(have<need&&Run&&GetTickCount64()<deadline){
  DWORD available=0;
  if(!PeekNamedPipe(pipe,nullptr,0,nullptr,&available,nullptr)){
   LastPipeError=GetLastError();return false;
  }
  if(!available){Sleep(5);continue;}
  DWORD take=std::min<DWORD>(available,need-have),got=0;
  if(!ReadFile(pipe,dst+have,take,&got,nullptr)||!got){
   LastPipeError=GetLastError();return false;
  }
  have+=got;
  deadline=GetTickCount64()+std::max<DWORD>(timeoutMs,1200);
 }
 if(have!=need)LastPipeError=WAIT_TIMEOUT;
 return have==need;
}
static bool readRpcPacket(HANDLE pipe,uint32_t&opcode,std::string&payload,DWORD timeoutMs){
 uint32_t header[2]={0,0};
 if(!readExactPipe(pipe,(char*)header,sizeof(header),timeoutMs))return false;
 opcode=header[0];uint32_t length=header[1];
 if(length>2*1024*1024){LastPipeError=ERROR_INVALID_DATA;return false;}
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
  setDiscordStatus(4,"Enter a valid Discord Application ID in Settings");
  return false;
 }
 if(DiscordState.load()==4)return false;
 setDiscordStatus(0,"Looking for Discord desktop...");
 bool pipeOpened=false;
 int lastFailure=0;
 std::string lastMessage;
 const std::string hello="{\"v\":1,\"client_id\":\""+DiscordClientId+"\"}";
 for(int i=0;i<10&&Run;i++){
  wchar_t name[96];
  swprintf(name,96,L"\\\\?\\pipe\\discord-ipc-%d",i);
  LastIpcPipe="discord-ipc-"+std::to_string(i);
  HANDLE h=INVALID_HANDLE_VALUE;
  for(int attempt=0;attempt<2&&Run;attempt++){
   h=CreateFileW(name,GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);
   if(h!=INVALID_HANDLE_VALUE)break;
   DWORD err=GetLastError();
   if(err!=ERROR_PIPE_BUSY)break;
   WaitNamedPipeW(name,350);
  }
  if(h==INVALID_HANDLE_VALUE)continue;
  pipeOpened=true;Pipe=h;
  DWORD mode=PIPE_READMODE_BYTE;
  SetNamedPipeHandleState(Pipe,&mode,nullptr,nullptr);
  setDiscordStatus(0,"Discord IPC found; waiting for READY...");
  LastPipeError=0;
  if(!pw(0,hello)){
   lastFailure=6;lastMessage="Could not send the Discord IPC handshake (Win32 error "+std::to_string(LastPipeError)+").";
   disconnectDiscord();continue;
  }
  const ULONGLONG deadline=GetTickCount64()+5000;
  while(Run&&Pipe!=INVALID_HANDLE_VALUE&&GetTickCount64()<deadline){
   uint32_t opcode=0;std::string response;
   DWORD remain=(DWORD)std::max<ULONGLONG>(1,deadline-GetTickCount64());
   if(!readRpcPacket(Pipe,opcode,response,std::min<DWORD>(remain,5000))){
    const DWORD err=LastPipeError;
    lastFailure=6;
    lastMessage="No READY reply from "+LastIpcPipe+". Check that Discord Desktop is open and the Application ID is valid";
    if(err&&err!=WAIT_TIMEOUT)lastMessage+=" (pipe error "+std::to_string(err)+")";
    break;
   }
   if(opcode==3){
    if(!pw(4,response)){lastFailure=6;lastMessage="Could not answer Discord's IPC ping.";break;}
    continue;
   }
   if(opcode==2){
    OutputDebugStringA(("Localify Discord handshake CLOSE: "+response).c_str());
    if(looksLikeInvalidClientId(response,true)){
     setDiscordStatus(4,"Discord rejected this Application ID. Open Settings and enter a valid ID.");
     disconnectDiscord();InvalidateRect(H,nullptr,FALSE);return false;
    }
    const int code=rpcCode(response);
    if(code==4001){lastFailure=5;lastMessage="Discord IPC CLOSE 4001: invalid origin.";}
    else if(code==4002){lastFailure=5;lastMessage="Discord IPC CLOSE 4002: rate limited; wait and retry.";}
    else if(code==4004){lastFailure=5;lastMessage="Discord IPC CLOSE 4004: invalid RPC version.";}
    else if(code==4005){lastFailure=5;lastMessage="Discord IPC CLOSE 4005: invalid encoding.";}
    else {lastFailure=5;lastMessage=describeDiscordResponse(response,"Discord closed the IPC handshake. Restart Discord and retry.");}
    break;
   }
   if(opcode!=1)continue;
   const std::string evt=js(response,"evt");
   if(evt=="READY"){
    Discord=true;setDiscordStatus(1,"Connected to Discord");
    InvalidateRect(H,nullptr,FALSE);return true;
   }
   if(evt=="ERROR"){
    OutputDebugStringA(("Localify Discord handshake ERROR: "+response).c_str());
    if(looksLikeInvalidClientId(response,false)){
     setDiscordStatus(4,"Discord rejected this Application ID. Open Settings and enter a valid ID.");
     disconnectDiscord();InvalidateRect(H,nullptr,FALSE);return false;
    }
    lastFailure=5;lastMessage=describeDiscordResponse(response,"Discord returned an IPC error during handshake.");break;
   }
  }
  if(Pipe!=INVALID_HANDLE_VALUE)disconnectDiscord();
 }
 Discord=false;
 if(lastFailure==0){
  if(pipeOpened){lastFailure=6;lastMessage="Discord's IPC pipe opened, but no READY reply arrived. Use Settings to verify the Application ID.";}
  else {lastFailure=2;lastMessage="Discord desktop app not found. Open Discord Desktop, then choose Reconnect.";}
 }
 setDiscordStatus(lastFailure,lastMessage);
 InvalidateRect(H,nullptr,FALSE);
 return false;
}

static bool sendRpcCommand(const std::string&json,const std::string&nonce){
 if(!connectDiscord()||!pw(1,json)){
  if(DiscordState.load()!=4)setDiscordStatus(5,"Discord RPC command could not be sent; reconnect the connector.");
  disconnectDiscord();return false;
 }
 const ULONGLONG deadline=GetTickCount64()+2500;
 while(Run&&GetTickCount64()<deadline){
  uint32_t opcode=0;std::string payload;
  DWORD remain=(DWORD)std::max<ULONGLONG>(1,deadline-GetTickCount64());
  if(!readRpcPacket(Pipe,opcode,payload,std::min<DWORD>(remain,400)))continue;
  if(opcode==3){if(!pw(4,payload)){setDiscordStatus(5,"Could not answer Discord's IPC ping.");disconnectDiscord();return false;}continue;}
  if(opcode==2){
   setDiscordStatus(5,describeDiscordResponse(payload,"Discord closed the RPC connection."));
   disconnectDiscord();return false;
  }
  if(opcode!=1)continue;
  if(js(payload,"nonce")!=nonce)continue;
  if(js(payload,"evt")=="ERROR"){
   const int code=rpcCode(payload);
   setDiscordStatus(5,describeDiscordResponse(payload,("Discord rejected the activity (RPC error "+std::to_string(code)+").").c_str()));
   disconnectDiscord();return false;
  }
  return true;
 }
 setDiscordStatus(6,"Discord did not confirm the activity update in time.");
 disconnectDiscord();
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
struct UiLayout{
 RECT card{},footer{},cover{},seek{},previous{},play{},next{},shuffle{},repeat{},volume{},openDiscord{},reconnect{},settings{};
};
static RECT makeRect(int l,int t,int r,int b){return RECT{l,t,r,b};}
static bool hit(RECT r,POINT p){return p.x>=r.left&&p.x<=r.right&&p.y>=r.top&&p.y<=r.bottom;}
static UiLayout layoutFor(int w,int h){
 UiLayout u;
 const int footerTop=h-88;
 const int cardTop=74,cardBottom=footerTop-14;
 u.card=makeRect(20,cardTop,w-20,cardBottom);
 u.footer=makeRect(20,footerTop,w-20,h-20);
 int coverSize=std::max(150,std::min(230,std::min((cardBottom-cardTop)-58,w/3-20)));
 u.cover=makeRect(36,cardTop+20,36+coverSize,cardTop+20+coverSize);
 const int infoX=u.cover.right+22,right=w-38;
 const int progressY=cardBottom-129;
 u.seek=makeRect(infoX,progressY-4,right,progressY+14);
 u.previous=makeRect(infoX,progressY+23,infoX+54,progressY+63);
 u.play=makeRect(infoX+64,progressY+23,infoX+134,progressY+63);
 u.next=makeRect(infoX+144,progressY+23,infoX+198,progressY+63);
 const int optionY=cardBottom-39;
 u.shuffle=makeRect(infoX,optionY,infoX+82,optionY+28);
 u.repeat=makeRect(infoX+92,optionY,infoX+172,optionY+28);
 u.volume=makeRect(infoX+215,optionY, right,optionY+28);
 const int btnY=footerTop+17;
 u.openDiscord=makeRect(w-345,btnY,w-239,btnY+34);
 u.reconnect=makeRect(w-229,btnY,w-125,btnY+34);
 u.settings=makeRect(w-115,btnY,w-22,btnY+34);
 return u;
}
static std::wstring durationText(double secs){
 int s=std::max(0,(int)std::floor(secs));
 wchar_t b[32]{};
 if(s>=3600)swprintf(b,32,L"%d:%02d:%02d",s/3600,(s/60)%60,s%60);
 else swprintf(b,32,L"%d:%02d",s/60,s%60);
 return b;
}
static void copyWideToClipboard(const std::wstring&text){
 if(!OpenClipboard(H))return;
 EmptyClipboard();
 HGLOBAL mem=GlobalAlloc(GMEM_MOVEABLE,(text.size()+1)*sizeof(wchar_t));
 if(mem){
  void*p=GlobalLock(mem);
  if(p){memcpy(p,text.c_str(),(text.size()+1)*sizeof(wchar_t));GlobalUnlock(mem);if(!SetClipboardData(CF_UNICODETEXT,mem))GlobalFree(mem);}
  else GlobalFree(mem);
 }
 CloseClipboard();
}
static std::wstring connectionDetails(){
 std::wstring d;
 {std::lock_guard<std::mutex>lock(DiscordStatusMutex);d=DiscordDiagnostic;}
 d=L"Localify Discord Connector\r\n\r\nStatus: "+(Discord.load()?std::wstring(L"Connected"):d);
 d+=L"\r\nApplication ID: "+(DiscordClientId.empty()?std::wstring(L"not configured"):W(DiscordClientId));
 d+=L"\r\nLast IPC pipe: "+W(LastIpcPipe);
 if(LastPipeError)d+=L"\r\nLast pipe error: "+std::to_wstring(LastPipeError);
 if(Server.load())d+=L"\r\nLocalify web bridge: online on 127.0.0.1:4219";
 else if(BridgeError.load()==WSAEADDRINUSE)d+=L"\r\nLocalify web bridge: port 4219 is already in use; close any older connector copy.";
 else d+=L"\r\nLocalify web bridge: not listening";
 d+=L"\r\n\r\nIf Discord rejects the Application ID, set the correct one in Settings.";
 return d;
}
static void openDiscordDesktop(){
 setDiscordStatus(0,"Opening Discord Desktop...");
 ShellExecuteW(nullptr,L"open",L"discord://",nullptr,nullptr,SW_SHOWNORMAL);
}
static bool saveClientId(const std::wstring&id){
 if(!validClientId(id)){
  MessageBoxW(SettingsWindow?SettingsWindow:H,L"Enter the numeric Application ID from your Discord Developer Portal. It should be 17–20 digits.",L"Invalid Application ID",MB_OK|MB_ICONWARNING);
  return false;
 }
 if(!WritePrivateProfileStringW(L"Discord",L"ClientId",id.c_str(),discordIniPath().c_str())){
  MessageBoxW(SettingsWindow?SettingsWindow:H,L"Could not save the connector settings file. Make sure this folder is writable.",L"Could not save settings",MB_OK|MB_ICONERROR);
  return false;
 }
 int bytes=WideCharToMultiByte(CP_UTF8,0,id.c_str(),-1,nullptr,0,nullptr,nullptr);
 if(bytes<=1)return false;
 std::string utf8((size_t)bytes,'\0');
 WideCharToMultiByte(CP_UTF8,0,id.c_str(),-1,utf8.data(),bytes,nullptr,nullptr);
 utf8.resize((size_t)bytes-1);DiscordClientId=utf8;
 setDiscordStatus(0,"Application ID saved; reconnecting...");
 DiscordReconnectRequested=true;
 return true;
}
static LRESULT CALLBACK settingsWndProc(HWND h,UINT msg,WPARAM w,LPARAM l){
 switch(msg){
  case WM_CREATE:{
   CreateWindowExW(0,L"STATIC",L"Discord connection settings",WS_CHILD|WS_VISIBLE,22,18,500,28,h,nullptr,GetModuleHandleW(nullptr),nullptr);
   CreateWindowExW(0,L"STATIC",L"Discord Application ID",WS_CHILD|WS_VISIBLE,24,65,490,20,h,nullptr,GetModuleHandleW(nullptr),nullptr);
   std::wstring current=DiscordClientId.empty()?W(CLIENT_ID):W(DiscordClientId);
   ClientIdEdit=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",current.c_str(),WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,22,88,518,30,h,(HMENU)(INT_PTR)2100,GetModuleHandleW(nullptr),nullptr);
   CreateWindowExW(0,L"STATIC",L"Required: open the Discord Developer Portal, create or select an application, copy its Application ID from General Information, paste it above, then choose Save & reconnect. A Discord user ID is different.",WS_CHILD|WS_VISIBLE,24,126,515,52,h,nullptr,GetModuleHandleW(nullptr),nullptr);
   KeepOnTopCheck=CreateWindowExW(0,L"BUTTON",L"Keep the connector above other windows",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,24,185,400,24,h,(HMENU)(INT_PTR)ID_SETTINGS_TOPMOST,GetModuleHandleW(nullptr),nullptr);
   SendMessageW(KeepOnTopCheck,BM_SETCHECK,KeepOnTop?BST_CHECKED:BST_UNCHECKED,0);
   CreateWindowExW(0,L"BUTTON",L"Open Discord",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,22,224,108,32,h,(HMENU)(INT_PTR)ID_SETTINGS_OPEN,GetModuleHandleW(nullptr),nullptr);
   CreateWindowExW(0,L"BUTTON",L"Copy details",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,138,224,108,32,h,(HMENU)(INT_PTR)ID_SETTINGS_COPY,GetModuleHandleW(nullptr),nullptr);
   CreateWindowExW(0,L"BUTTON",L"Open Developer Portal",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,22,273,155,34,h,(HMENU)(INT_PTR)ID_SETTINGS_DEFAULT,GetModuleHandleW(nullptr),nullptr);
   CreateWindowExW(0,L"BUTTON",L"Cancel",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,358,273,78,34,h,(HMENU)(INT_PTR)ID_SETTINGS_CANCEL,GetModuleHandleW(nullptr),nullptr);
   CreateWindowExW(0,L"BUTTON",L"Save & reconnect",WS_CHILD|WS_VISIBLE|BS_DEFPUSHBUTTON,444,273,110,34,h,(HMENU)(INT_PTR)ID_SETTINGS_SAVE,GetModuleHandleW(nullptr),nullptr);
   return 0;
  }
  case WM_COMMAND:{
   const int id=LOWORD(w);
   if(id==ID_SETTINGS_SAVE){
    wchar_t idText[64]{};GetWindowTextW(ClientIdEdit,idText,64);
    if(saveClientId(idText))DestroyWindow(h);
    return 0;
   }
   if(id==ID_SETTINGS_DEFAULT){ShellExecuteW(nullptr,L"open",L"https://discord.com/developers/applications",nullptr,nullptr,SW_SHOWNORMAL);return 0;}
   if(id==ID_SETTINGS_CANCEL){DestroyWindow(h);return 0;}
   if(id==ID_SETTINGS_OPEN){openDiscordDesktop();return 0;}
   if(id==ID_SETTINGS_COPY){copyWideToClipboard(connectionDetails());MessageBoxW(h,L"Connection details copied to the clipboard.",L"Localify",MB_OK|MB_ICONINFORMATION);return 0;}
   if(id==ID_SETTINGS_TOPMOST){
    KeepOnTop=SendMessageW(KeepOnTopCheck,BM_GETCHECK,0,0)==BST_CHECKED;
    if(H)SetWindowPos(H,KeepOnTop?HWND_TOPMOST:HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
    return 0;
   }
   break;
  }
  case WM_CLOSE:DestroyWindow(h);return 0;
  case WM_DESTROY:
   if(h==SettingsWindow){SettingsWindow=nullptr;ClientIdEdit=nullptr;KeepOnTopCheck=nullptr;if(H){EnableWindow(H,TRUE);SetForegroundWindow(H);}}
   return 0;
 }
 return DefWindowProcW(h,msg,w,l);
}
static void openSettingsWindow(){
 if(SettingsWindow&&IsWindow(SettingsWindow)){ShowWindow(SettingsWindow,SW_SHOWNORMAL);SetForegroundWindow(SettingsWindow);return;}
 if(!H)return;
 RECT owner{};GetWindowRect(H,&owner);const int ww=580,hh=355;
 const int x=owner.left+((owner.right-owner.left)-ww)/2;
 const int y=owner.top+((owner.bottom-owner.top)-hh)/2;
 EnableWindow(H,FALSE);
 SettingsWindow=CreateWindowExW(WS_EX_DLGMODALFRAME,L"LocalifyPlayerSettings",L"Localify Discord Connector — Settings",
   WS_CAPTION|WS_SYSMENU|WS_POPUP,x,y,ww,hh,H,nullptr,GetModuleHandleW(nullptr),nullptr);
 if(!SettingsWindow){EnableWindow(H,TRUE);return;}
 BOOL dark=TRUE;DwmSetWindowAttribute(SettingsWindow,DWMWA_USE_IMMERSIVE_DARK_MODE,&dark,sizeof(dark));
 ShowWindow(SettingsWindow,SW_SHOWNORMAL);UpdateWindow(SettingsWindow);SetForegroundWindow(SettingsWindow);
}
static void paint(HDC out){
 RECT rr{};GetClientRect(H,&rr);const int w=rr.right,h=rr.bottom;
 if(w<40||h<40)return;
 HDC m=CreateCompatibleDC(out);if(!m)return;
 HBITMAP bm=CreateCompatibleBitmap(out,w,h);if(!bm){DeleteDC(m);return;}
 HGDIOBJ old=SelectObject(m,bm);
 Graphics g(m);g.SetSmoothingMode(SmoothingModeAntiAlias);g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
 g.Clear(Color(255,7,9,13));
 fill(g,10,10,(float)w-20,(float)h-20,20,Color(255,13,16,22));
 SolidBrush glow(Color(13,72,151,221));g.FillEllipse(&glow,-65,-95,360,220);
 fill(g,24,18,37,31,10,Color(255,61,145,222));
 center(g,L"L",24,18,37,31,18,Color(255,255,255,255),true);
 txt(g,L"LOCALIFY",72,17,180,23,17,Color(255,244,247,252),true);
 txt(g,L"DISCORD CONNECTOR",73,39,190,16,9,Color(255,122,136,155),true);
 std::wstring bridge=Server.load()?L"WEB BRIDGE ONLINE":(BridgeError.load()==WSAEADDRINUSE?L"BRIDGE PORT IN USE":L"STARTING LOCAL BRIDGE");
 const Color bridgeColor=Server.load()?Color(255,96,208,155):(BridgeError.load()?Color(255,255,151,124):Color(255,230,187,102));
 fill(g,(float)w-214,23, (float)w-27,49,12,Color(255,21,27,35));
 txt(g,bridge.c_str(),(float)w-203,30,168,14,8,bridgeColor,true);

 Presence p;{std::lock_guard<std::mutex> lock(M);p=P;}
 const UiLayout u=layoutFor(w,h);
 fill(g,(float)u.card.left,(float)u.card.top,(float)(u.card.right-u.card.left),(float)(u.card.bottom-u.card.top),18,Color(255,17,21,29));
 const int coverW=u.cover.right-u.cover.left,coverH=u.cover.bottom-u.cover.top;
 fill(g,(float)u.cover.left,(float)u.cover.top,(float)coverW,(float)coverH,16,Color(255,27,33,43));
 {
  std::lock_guard<std::mutex> lock(M);
  if(Cover)g.DrawImage(Cover.get(),RectF((REAL)u.cover.left,(REAL)u.cover.top,(REAL)coverW,(REAL)coverH));
  else{
   center(g,L"♫",(float)u.cover.left,(float)u.cover.top+8,(float)coverW,(float)coverH-18,44,Color(255,104,178,238),true);
   center(g,L"ALBUM ART", (float)u.cover.left,(float)u.cover.bottom-28,(float)coverW,16,8,Color(255,114,128,148),true);
  }
 }
 const float infoX=(float)u.cover.right+22;
 const float infoW=(float)w-infoX-38;
 std::wstring song=p.song.empty()?L"Nothing playing":W(p.song);
 std::wstring artist=W(p.artist.empty()?"Play a song in Localify to show it here":p.artist);
 std::wstring album=W(p.album.empty()?"Local library":p.album);
 txt(g,L"NOW PLAYING",infoX,(float)u.card.top+26,170,16,9,Color(255,109,174,232),true);
 txt(g,song.c_str(),infoX,(float)u.card.top+48,infoW,38,22,Color(255,246,248,252),true);
 txt(g,artist.c_str(),infoX,(float)u.card.top+88,infoW,24,12,Color(255,178,187,201));
 txt(g,album.c_str(),infoX,(float)u.card.top+115,infoW,21,10,Color(255,122,135,153));
 const bool isPlaying=p.playing;
 fill(g,infoX,(float)u.card.top+145,isPlaying?100:84,(float)u.card.top+171-((float)u.card.top+145),10,isPlaying?Color(255,20,54,47):Color(255,30,36,46));
 center(g,isPlaying?L"●  PLAYING":L"Ⅱ  PAUSED",infoX,(float)u.card.top+145,isPlaying?100:84,26,9,isPlaying?Color(255,124,226,175):Color(255,161,174,192),true);

 const float barY=(float)u.seek.top+7;
 fill(g,(float)u.seek.left,barY,(float)(u.seek.right-u.seek.left),5,3,Color(255,42,49,61));
 const double ratio=p.dur>0?std::clamp(p.pos/p.dur,0.0,1.0):0.0;
 if(ratio>0)fill(g,(float)u.seek.left,barY,(float)(u.seek.right-u.seek.left)*(float)ratio,5,3,Color(255,91,170,244));
 const std::wstring elapsed=durationText(p.pos),duration=durationText(p.dur);
 txt(g,elapsed.c_str(),(float)u.seek.left,(float)u.seek.top-17,70,14,8,Color(255,125,139,157));
 txt(g,duration.c_str(),(float)u.seek.right-70,(float)u.seek.top-17,70,14,8,Color(255,125,139,157));
 auto drawBtn=[&](RECT r,const wchar_t*label,bool active,bool accent,int size){
  fill(g,(float)r.left,(float)r.top,(float)(r.right-r.left),(float)(r.bottom-r.top),9,accent?Color(255,83,159,231):(active?Color(255,31,54,67):Color(255,27,33,42)));
  center(g,label,(float)r.left,(float)r.top,(float)(r.right-r.left),(float)(r.bottom-r.top),(float)size,accent?Color(255,8,17,28):(active?Color(255,130,195,245):Color(255,209,216,227)),true);
 };
 drawBtn(u.previous,L"PREV",false,false,9);
 drawBtn(u.play,p.playing?L"Ⅱ  PAUSE":L"▶  PLAY",false,true,10);
 drawBtn(u.next,L"NEXT",false,false,9);
 drawBtn(u.shuffle,L"SHUFFLE",p.shuffle,false,8);
 drawBtn(u.repeat,p.repeat=="one"?L"REPEAT 1":L"REPEAT",p.repeat!="off",false,8);
 txt(g,L"VOL", (float)u.volume.left-33,(float)u.volume.top+6,28,15,8,Color(255,123,137,155),true);
 fill(g,(float)u.volume.left,(float)u.volume.top+11,(float)(u.volume.right-u.volume.left),5,3,Color(255,42,49,61));
 const double vol=std::clamp(p.volume,0.0,1.0);
 if(vol>0)fill(g,(float)u.volume.left,(float)u.volume.top+11,(float)(u.volume.right-u.volume.left)*(float)vol,5,3,Color(255,91,170,244));
 fill(g,(float)u.volume.left+(float)(u.volume.right-u.volume.left)*(float)vol-4,(float)u.volume.top+6,8,15,4,Color(255,227,240,255));
 wchar_t volPct[16]{};swprintf(volPct,16,L"%d%%",(int)std::lround(vol*100));
 txt(g,volPct,(float)u.volume.right-33,(float)u.volume.top-13,33,12,8,Color(255,140,153,171),true);

 fill(g,(float)u.footer.left,(float)u.footer.top,(float)(u.footer.right-u.footer.left),(float)(u.footer.bottom-u.footer.top),16,Color(255,17,21,29));
 const int ds=DiscordState.load();
 const Color dot=Discord.load()?Color(255,76,210,149):((ds==4||ds==5||ds==6)?Color(255,255,115,100):Color(255,234,185,93));
 SolidBrush statusDot(dot);g.FillEllipse(&statusDot,36.0f,(REAL)u.footer.top+17.0f,9.0f,9.0f);
 txt(g,L"DISCORD CONNECTION",53,(float)u.footer.top+11,170,15,9,Color(255,216,222,232),true);
 std::wstring diagnostic;{std::lock_guard<std::mutex> lock(DiscordStatusMutex);diagnostic=DiscordDiagnostic;}
 if(Discord.load())diagnostic=L"Connected — listening activity is ready";
 else if(diagnostic.empty())diagnostic=L"Open Discord Desktop, then press Reconnect";
 if(BridgeError.load()==WSAEADDRINUSE)diagnostic=L"Bridge port 4219 is busy. Close older connector copies.";
 txt(g,diagnostic.c_str(),36,(float)u.footer.top+33,(float)u.openDiscord.left-48,17,9,dot);
 drawBtn(u.openDiscord,L"OPEN DISCORD",false,false,8);
 drawBtn(u.reconnect,L"RECONNECT",false,false,8);
 drawBtn(u.settings,L"SETTINGS",false,false,8);
 BitBlt(out,0,0,w,h,m,0,0,SRCCOPY);
 SelectObject(m,old);DeleteObject(bm);DeleteDC(m);
}
static LRESULT CALLBACK wnd(HWND h,UINT msg,WPARAM w,LPARAM l){
 switch(msg){
  case WM_ERASEBKGND:return 1;
  case WM_GETMINMAXINFO:{
   MINMAXINFO*mm=(MINMAXINFO*)l;mm->ptMinTrackSize.x=780;mm->ptMinTrackSize.y=500;return 0;
  }
  case WM_SIZE:InvalidateRect(h,nullptr,FALSE);return 0;
  case WM_PAINT:{
   PAINTSTRUCT ps;HDC dc=BeginPaint(h,&ps);paint(dc);EndPaint(h,&ps);return 0;
  }
  case WM_LBUTTONDOWN:{
   POINT p{GET_X_LPARAM(l),GET_Y_LPARAM(l)};RECT rc{};GetClientRect(h,&rc);const UiLayout u=layoutFor(rc.right,rc.bottom);
   if(hit(u.openDiscord,p)){openDiscordDesktop();return 0;}
   if(hit(u.reconnect,p)){setDiscordStatus(0,"Reconnect requested...");DiscordReconnectRequested=true;InvalidateRect(h,nullptr,FALSE);return 0;}
   if(hit(u.settings,p)){openSettingsWindow();return 0;}
   if(p.y>=u.footer.top&&p.y<=u.footer.bottom&&p.x<u.openDiscord.left-8){MessageBoxW(h,connectionDetails().c_str(),L"Discord connection details",MB_OK|MB_ICONINFORMATION);return 0;}
   if(hit(u.previous,p)){issue("prev");return 0;}
   if(hit(u.play,p)){issue("toggle");return 0;}
   if(hit(u.next,p)){issue("next");return 0;}
   if(hit(u.shuffle,p)){issue("shuffle");return 0;}
   if(hit(u.repeat,p)){issue("repeat");return 0;}
   if(hit(u.seek,p)){
    DragSeeking=true;SetCapture(h);
    double d;{std::lock_guard<std::mutex> lock(M);d=P.dur;}
    if(d>0)issue("seek",std::clamp((p.x-u.seek.left)/(double)std::max(1.0,(double)(u.seek.right-u.seek.left)),0.0,1.0)*d);
    return 0;
   }
   if(hit(u.volume,p)){
    DragVolume=true;SetCapture(h);
    issue("volume",std::clamp((p.x-u.volume.left)/(double)std::max(1.0,(double)(u.volume.right-u.volume.left)),0.0,1.0));
    return 0;
   }
   return 0;
  }
  case WM_MOUSEMOVE:{
   if(!DragSeeking&&!DragVolume)return 0;
   POINT p{GET_X_LPARAM(l),GET_Y_LPARAM(l)};RECT rc{};GetClientRect(h,&rc);const UiLayout u=layoutFor(rc.right,rc.bottom);
   if(DragSeeking){
    double d;{std::lock_guard<std::mutex> lock(M);d=P.dur;}
    if(d>0)issue("seek",std::clamp((p.x-u.seek.left)/(double)std::max(1.0,(double)(u.seek.right-u.seek.left)),0.0,1.0)*d);
   }else{
    issue("volume",std::clamp((p.x-u.volume.left)/(double)std::max(1.0,(double)(u.volume.right-u.volume.left)),0.0,1.0));
   }
   return 0;
  }
  case WM_LBUTTONUP:
   if(DragSeeking||DragVolume){DragSeeking=false;DragVolume=false;ReleaseCapture();return 0;}return 0;
  case WM_MOUSEWHEEL:{
   RECT rc{};GetClientRect(h,&rc);const UiLayout u=layoutFor(rc.right,rc.bottom);
   POINT p{GET_X_LPARAM(l),GET_Y_LPARAM(l)};ScreenToClient(h,&p);
   if(hit(u.volume,p)){
    double v;{std::lock_guard<std::mutex> lock(M);v=P.volume;}
    v=std::clamp(v+(GET_WHEEL_DELTA_WPARAM(w)>0?0.05:-0.05),0.0,1.0);issue("volume",v);return 0;
   }
   break;
  }
  case WM_KEYDOWN:
   if(w==VK_SPACE){issue("toggle");return 0;}
   if(w==VK_LEFT){issue("prev");return 0;}
   if(w==VK_RIGHT){issue("next");return 0;}
   if(w==VK_F5){setDiscordStatus(0,"Reconnect requested...");DiscordReconnectRequested=true;return 0;}
   break;
  case WM_TIMER:InvalidateRect(h,nullptr,FALSE);return 0;
  case WM_CLOSE:DestroyWindow(h);return 0;
  case WM_DESTROY:Run=false;PostQuitMessage(0);return 0;
 }
 return DefWindowProcW(h,msg,w,l);
}
int WINAPI wWinMain(HINSTANCE hi,HINSTANCE,LPWSTR,int show){
 GdiplusStartupInput gi;if(GdiplusStartup(&GP,&gi,0)!=Ok)return 1;
 loadDiscordClientId();WSADATA wd;WSAStartup(MAKEWORD(2,2),&wd);
 WNDCLASSEXW sc{};sc.cbSize=sizeof(sc);sc.hInstance=hi;sc.lpfnWndProc=settingsWndProc;sc.lpszClassName=L"LocalifyPlayerSettings";sc.hCursor=LoadCursor(0,IDC_ARROW);sc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);RegisterClassExW(&sc);
 WNDCLASSEXW wc{};wc.cbSize=sizeof(wc);wc.hInstance=hi;wc.lpfnWndProc=wnd;wc.lpszClassName=L"LocalifyPlayer5";wc.hCursor=LoadCursor(0,IDC_ARROW);wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);RegisterClassExW(&wc);
 H=CreateWindowExW(WS_EX_APPWINDOW,L"LocalifyPlayer5",L"Localify Discord Connector",
  WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,CW_USEDEFAULT,CW_USEDEFAULT,900,560,0,0,hi,0);
 if(!H)return 1;
 BOOL dark=TRUE;DwmSetWindowAttribute(H,DWMWA_USE_IMMERSIVE_DARK_MODE,&dark,sizeof(dark));
 ShowWindow(H,show);UpdateWindow(H);SetTimer(H,1,350,0);
 std::thread(server).detach();std::thread(rpcLoop).detach();
 MSG msg;while(GetMessageW(&msg,0,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}
 Run=false;disconnectDiscord();WSACleanup();GdiplusShutdown(GP);return 0;
}
// Localify Discord Connector 5.1 — redesigned controls and explicit Application ID setup.
