#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>
namespace mgr_detail {
struct Error : std::runtime_error { int code; Error(int c, const std::string& s):runtime_error(s),code(c){} };
inline void require(bool b,int code,const std::string& why){if(!b)throw Error(code,why);}
struct J {
 using A=std::vector<J>; using O=std::map<std::string,J>;
 std::variant<std::nullptr_t,bool,int64_t,std::string,A,O> v;
 J():v(nullptr){} J(std::nullptr_t):v(nullptr){} J(bool x):v(x){} J(int x):v(int64_t(x)){} J(uint32_t x):v(int64_t(x)){} J(int64_t x):v(x){} J(const char*x):v(std::string(x)){} J(std::string x):v(std::move(x)){} J(A x):v(std::move(x)){} J(O x):v(std::move(x)){}
 bool object()const{return std::holds_alternative<O>(v);} bool array()const{return std::holds_alternative<A>(v);} bool string()const{return std::holds_alternative<std::string>(v);}
 const O& obj()const{require(object(),4,"expected_object");return std::get<O>(v);} O& obj(){require(object(),4,"expected_object");return std::get<O>(v);}
 const A& arr()const{require(array(),4,"expected_array");return std::get<A>(v);} A& arr(){require(array(),4,"expected_array");return std::get<A>(v);}
 const std::string& str()const{require(string(),4,"expected_string");return std::get<std::string>(v);}
 int64_t num()const{require(std::holds_alternative<int64_t>(v),4,"expected_safe_integer");return std::get<int64_t>(v);}
 bool boolean()const{require(std::holds_alternative<bool>(v),4,"expected_boolean");return std::get<bool>(v);}
 bool has(const std::string&k)const{return object()&&obj().count(k);}
 const J& at(const std::string&k)const{auto it=obj().find(k);require(it!=obj().end(),4,"missing_"+k);return it->second;}
 J& operator[](const std::string&k){return obj()[k];}
 std::string text(const std::string&k,std::string def="")const{return has(k)?at(k).str():def;}
 int64_t number(const std::string&k,int64_t def=0)const{return has(k)?at(k).num():def;}
 bool flag(const std::string&k,bool def=false)const{return has(k)?at(k).boolean():def;}
};
inline bool utf8(const std::string&s){for(size_t i=0;i<s.size();){uint32_t c=(uint8_t)s[i++];if(c<128)continue;int n=c>=0xc2&&c<=0xdf?1:c>=0xe0&&c<=0xef?2:c>=0xf0&&c<=0xf4?3:-1;if(n<0||i+n>s.size())return false;uint32_t cp=c&((1u<<(6-n))-1);for(int j=0;j<n;j++){auto d=(uint8_t)s[i++];if((d&0xc0)!=0x80)return false;cp=cp*64+(d&63);}if(cp<(n==1?128u:n==2?2048u:65536u)||cp>0x10ffff||(cp>=0xd800&&cp<=0xdfff))return false;}return true;}
inline void append_utf8(std::string&s,uint32_t c){if(c<128)s+=char(c);else if(c<2048){s+=char(0xc0|(c>>6));s+=char(0x80|(c&63));}else if(c<65536){s+=char(0xe0|(c>>12));s+=char(0x80|((c>>6)&63));s+=char(0x80|(c&63));}else{s+=char(0xf0|(c>>18));s+=char(0x80|((c>>12)&63));s+=char(0x80|((c>>6)&63));s+=char(0x80|(c&63));}}
struct Parser {
 const std::string&s; size_t p=0,nodes=0; explicit Parser(const std::string&x):s(x){require(x.size()<=16*1024*1024&&utf8(x),4,"json_size_or_utf8");}
 void ws(){while(p<s.size()&&(s[p]==' '||s[p]=='\n'||s[p]=='\r'||s[p]=='\t'))p++;}
 char take(){require(p<s.size(),4,"json_truncated");return s[p++];}
 uint32_t hex4(){uint32_t v=0;for(int i=0;i<4;i++){char c=take();require((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F'),4,"json_escape_hex");v=v*16+(c<='9'?c-'0':(c|32)-'a'+10);}return v;}
 std::string string(){require(take()=='"',4,"json_string");std::string out;for(;;){char c=take();if(c=='"')return out;require((uint8_t)c>=32,4,"json_control");if(c!='\\'){out+=c;continue;}c=take();switch(c){case '"':case '\\':case '/':out+=c;break;case 'b':out+='\b';break;case 'f':out+='\f';break;case 'n':out+='\n';break;case 'r':out+='\r';break;case 't':out+='\t';break;case 'u':{auto cp=hex4();if(cp>=0xd800&&cp<=0xdbff){require(take()=='\\'&&take()=='u',4,"json_surrogate");auto lo=hex4();require(lo>=0xdc00&&lo<=0xdfff,4,"json_surrogate");cp=0x10000+(cp-0xd800)*1024+lo-0xdc00;}else require(cp<0xdc00||cp>0xdfff,4,"json_surrogate");append_utf8(out,cp);break;}default:throw Error(4,"json_escape");}}}
 J value(int depth=0){require(depth<=64&&++nodes<=200000,4,"json_complexity");ws();require(p<s.size(),4,"json_empty");char c=s[p];if(c=='"')return string();if(c=='{'){p++;J::O out;ws();if(p<s.size()&&s[p]=='}'){p++;return out;}for(;;){ws();auto k=string();ws();require(take()==':',4,"json_colon");auto val=value(depth+1);require(out.emplace(k,std::move(val)).second,4,"json_duplicate_key");ws();char sep=take();if(sep=='}')return out;require(sep==',',4,"json_object_separator");}}
 if(c=='['){p++;J::A out;ws();if(p<s.size()&&s[p]==']'){p++;return out;}for(;;){out.push_back(value(depth+1));require(out.size()<=65536,4,"json_array_limit");ws();char sep=take();if(sep==']')return out;require(sep==',',4,"json_array_separator");}}
 for(auto lit:{"true","false","null"}){size_t n=std::char_traits<char>::length(lit);if(s.compare(p,n,lit)==0){p+=n;return lit[0]=='n'?J():J(lit[0]=='t');}}
 size_t begin=p;bool neg=c=='-';if(neg)p++;require(p<s.size()&&s[p]>='0'&&s[p]<='9',4,"json_integer");if(s[p]=='0')p++;else while(p<s.size()&&s[p]>='0'&&s[p]<='9')p++;require(p==s.size()||(s[p]!='.'&&s[p]!='e'&&s[p]!='E'),4,"json_float_use_typed_bits");std::string token=s.substr(begin,p-begin);try{auto x=std::stoll(token);require(x>=-9007199254740991LL&&x<=9007199254740991LL,4,"json_unsafe_integer_use_typed_string");return J(int64_t(x));}catch(const std::invalid_argument&){throw Error(4,"json_integer");}catch(const std::out_of_range&){throw Error(4,"json_integer_range");}}
 J parse(){auto out=value();ws();require(p==s.size(),4,"json_trailing");return out;}
};
inline std::string quote(const std::string&s){std::string out="\"";static const char*h="0123456789abcdef";for(uint8_t c:s){switch(c){case '"':out+="\\\"";break;case '\\':out+="\\\\";break;case '\b':out+="\\b";break;case '\f':out+="\\f";break;case '\n':out+="\\n";break;case '\r':out+="\\r";break;case '\t':out+="\\t";break;default:if(c<32){out+="\\u00";out+=h[c>>4];out+=h[c&15];}else out+=char(c);}}return out+'"';}
inline std::string dump(const J&j){if(std::holds_alternative<std::nullptr_t>(j.v))return "null";if(std::holds_alternative<bool>(j.v))return j.boolean()?"true":"false";if(std::holds_alternative<int64_t>(j.v))return std::to_string(j.num());if(j.string())return quote(j.str());std::string out=j.array()?"[":"{";bool first=true;if(j.array()){for(auto&v:j.arr()){if(!first)out+=',';first=false;out+=dump(v);}}else for(auto&v:j.obj()){if(!first)out+=',';first=false;out+=quote(v.first)+':'+dump(v.second);}return out+(j.array()?"]":"}");}
inline std::string read(const std::filesystem::path&p,size_t max=64*1024*1024){std::error_code ec;auto n=std::filesystem::file_size(p,ec);require(!ec&&n<=max,9,"file_missing_or_too_large:"+p.generic_string());std::ifstream f(p,std::ios::binary);std::string s(size_t(n),'\0');f.read(s.data(),std::streamsize(n));require(bool(f)||n==0,9,"file_read");return s;}
inline J load(const std::filesystem::path&p){return Parser(read(p,16*1024*1024)).parse();}
inline void write(const std::filesystem::path&p,const std::string&s){if(!p.parent_path().empty())std::filesystem::create_directories(p.parent_path());std::ofstream f(p,std::ios::binary);f.write(s.data(),std::streamsize(s.size()));require(bool(f),9,"file_write");}
inline std::string sha256(const std::string&s){static constexpr uint32_t k[64]={0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};uint32_t h[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};std::string b=s;b+=char(128);while(b.size()%64!=56)b+='\0';uint64_t bits=uint64_t(s.size())*8;for(int i=7;i>=0;i--)b+=char(bits>>(i*8));auto rotr=[](uint32_t x,int n){return (x>>n)|(x<<(32-n));};for(size_t p=0;p<b.size();p+=64){uint32_t w[64];for(int i=0;i<16;i++)w[i]=(uint32_t(uint8_t(b[p+4*i]))<<24)|(uint32_t(uint8_t(b[p+4*i+1]))<<16)|(uint32_t(uint8_t(b[p+4*i+2]))<<8)|uint8_t(b[p+4*i+3]);for(int i=16;i<64;i++)w[i]=w[i-16]+(rotr(w[i-15],7)^rotr(w[i-15],18)^(w[i-15]>>3))+w[i-7]+(rotr(w[i-2],17)^rotr(w[i-2],19)^(w[i-2]>>10));uint32_t a=h[0],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7],bb=h[1];for(int i=0;i<64;i++){uint32_t t1=hh+(rotr(e,6)^rotr(e,11)^rotr(e,25))+((e&f)^(~e&g))+k[i]+w[i];uint32_t t2=(rotr(a,2)^rotr(a,13)^rotr(a,22))+((a&bb)^(a&c)^(bb&c));hh=g;g=f;f=e;e=d+t1;d=c;c=bb;bb=a;a=t1+t2;}uint32_t result[8]={a,bb,c,d,e,f,g,hh};for(int i=0;i<8;i++)h[i]+=result[i];}std::ostringstream out;out<<std::hex<<std::setfill('0');for(auto x:h)out<<std::setw(8)<<x;return out.str();}
inline std::string hash(const J&j){return sha256(dump(j));}
inline bool valid_hash(const std::string&s){return s.size()==64&&s!=std::string(64,'0')&&std::all_of(s.begin(),s.end(),[](char c){return(c>='0'&&c<='9')||(c>='a'&&c<='f');});}
inline void keys(const J&j,std::initializer_list<const char*> allowed){for(auto&[k,v]:j.obj()){bool ok=false;for(auto*a:allowed)if(k==a)ok=true;require(ok,4,"unknown_field:"+k);}}
inline std::filesystem::path within(const std::filesystem::path&root,const std::string&rel){std::filesystem::path p=std::filesystem::u8path(rel);require(!rel.empty()&&!p.is_absolute()&&rel.find(':')==std::string::npos&&rel.find('\\')==std::string::npos,4,"path_relative_required");for(const auto&part:p)require(part!=".."&&part!=".",4,"path_traversal");auto base=std::filesystem::weakly_canonical(root);auto target=std::filesystem::weakly_canonical(base/p);auto bi=base.begin(),ti=target.begin();for(;bi!=base.end();++bi,++ti)require(ti!=target.end()&&*bi==*ti,4,"path_escape");return target;}
}
