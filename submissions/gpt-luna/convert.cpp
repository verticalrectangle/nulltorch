#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>
using namespace std; namespace fs=std::filesystem; using U=uint64_t;
struct V; using P=shared_ptr<V>; struct V{enum K{N,I,S,B,T,L,D,G,ST,X}k=N; long long n=0; string s,m,nm,key,dt; vector<P>a; map<string,P>d; long long off=0;vector<long long>sh,st;};
P vv(V::K k){auto p=make_shared<V>();p->k=k;return p;} P sv(string s){auto p=vv(V::S);p->s=s;return p;} P iv(long long n){auto p=vv(V::I);p->n=n;return p;} string ss(P p){return p&&p->k==V::S?p->s:"";}
struct E{string n;U cs=0,us=0,off=0;uint16_t meth=0;vector<uint8_t> ex;};
struct Zip{vector<uint8_t>b;vector<E>e;uint16_t w(U p){return b.at(p)|b.at(p+1)<<8;}uint32_t d(U p){return b.at(p)|b.at(p+1)<<8|b.at(p+2)<<16|b.at(p+3)<<24;}U q(U p){U x=0;for(int i=7;i>=0;i--)x=x<<8|b.at(p+i);return x;}
 void extra(vector<uint8_t>&x,uint32_t cs,uint32_t us,uint32_t of,U&c,U&u,U&o){for(U p=0;p+4<=x.size();){uint16_t id=wvec(x,p),n=wvec(x,p+2);p+=4;if(p+n>x.size())break;if(id==1){U z=p;if(us==0xffffffff){u=qvec(x,z);z+=8;}if(cs==0xffffffff){c=qvec(x,z);z+=8;}if(of==0xffffffff)o=qvec(x,z);}p+=n;}}
 uint16_t wvec(vector<uint8_t>&x,U p){return x[p]|x[p+1]<<8;}U qvec(vector<uint8_t>&x,U p){U z=0;for(int i=7;i>=0;i--)z=z<<8|x[p+i];return z;}
 Zip(string path){ifstream f(path,ios::binary);b.assign(istreambuf_iterator<char>(f),{});U ep=U(-1),lo=b.size()>65557?b.size()-65557:0;for(U p=b.size()>=22?b.size()-22:0;;p--){if(p+22<=b.size()&&d(p)==0x06054b50&&p+22+w(p+20)<=b.size()){ep=p;break;}if(p==lo||p==0)break;}if(ep==U(-1))throw runtime_error("eocd");U cnt=w(ep+10),co=d(ep+16);if(cnt==0xffff||co==0xffffffff){if(ep<20||d(ep-20)!=0x07064b50)throw runtime_error("zip64");U z=q(ep-12);cnt=q(z+32);co=q(z+48);}for(U i=0,p=co;i<cnt;i++){if(d(p)!=0x02014b50)throw runtime_error("central");uint16_t nl=w(p+28),ne=w(p+30),nk=w(p+32);uint32_t cs=d(p+20),us=d(p+24),of=d(p+42);E x;x.n=string((char*)&b[p+46],nl);x.ex.assign(b.begin()+p+46+nl,b.begin()+p+46+nl+ne);x.cs=cs;x.us=us;x.off=of;x.meth=w(p+10);if(cs==0xffffffff||us==0xffffffff||of==0xffffffff){U c=cs,u=us,o=of;extra(x.ex,cs,us,of,c,u,o);x.cs=c;x.us=u;x.off=o;}e.push_back(x);p+=46+nl+ne+nk;}}
 vector<uint8_t> get(string n){for(auto&x:e)if(x.n==n){if(x.meth!=0)throw runtime_error("deflate");if(d(x.off)!=0x04034b50)throw runtime_error("local");U z=x.off+30+w(x.off+26)+w(x.off+28);if(z+x.cs>b.size())throw runtime_error("truncated");return vector<uint8_t>(b.begin()+z,b.begin()+z+x.cs);}throw runtime_error("entry "+n);}
 string pkl(){for(auto&x:e)if(x.n.size()>=8&&x.n.substr(x.n.size()-8)=="data.pkl")return x.n;throw runtime_error("data.pkl");}
};
struct Pick{vector<uint8_t>&b;size_t p=0;vector<P>st,mem;vector<size_t>mk;Pick(vector<uint8_t>&x):b(x){}uint8_t g(){if(p>=b.size())throw runtime_error("pickle");return b[p++];}uint32_t i4(){uint32_t x=0;for(int i=0;i<4;i++)x|=uint32_t(g())<<(8*i);return x;}uint64_t i8(){uint64_t x=0;for(int i=0;i<8;i++)x|=uint64_t(g())<<(8*i);return x;}string raw(U n){if(p+n>b.size())throw runtime_error("pickle");string s((char*)&b[p],n);p+=n;return s;}string line(){string s;while(p<b.size()&&b[p]!='\n')s+=b[p++];if(p==b.size())throw runtime_error("pickle");p++;return s;}P pop(){if(st.empty())throw runtime_error("stack");P x=st.back();st.pop_back();return x;}void memo(U n,P x){if(n>=mem.size())mem.resize(n+1);mem[n]=x;}vector<P> marked(){if(mk.empty())throw runtime_error("mark");U z=mk.back();mk.pop_back();vector<P>x(st.begin()+z+1,st.end());st.resize(z);return x;}
 P reduce(P fn,P ar){if(!fn||fn->k!=V::G||!ar||ar->k!=V::T)return vv(V::N);if(fn->nm=="_rebuild_tensor_v2"||fn->nm=="_rebuild_tensor"){auto x=vv(V::X);if(ar->a.size()>=4){P z=ar->a[0];if(z->k==V::ST){x->key=z->key;x->dt=z->dt;}x->off=ar->a[1]->n;for(P y:ar->a[2]->a)x->sh.push_back(y->n);for(P y:ar->a[3]->a)x->st.push_back(y->n);}return x;}if(fn->nm=="OrderedDict"||fn->nm=="dict"){auto x=vv(V::D);if(!ar->a.empty()&&ar->a[0]->k==V::L)for(P y:ar->a[0]->a)if(y->k==V::T&&y->a.size()>1)x->d[ss(y->a[0])]=y->a[1];return x;}if(fn->m=="os"||fn->m=="posix"||fn->nm=="system"||fn->nm=="popen"||fn->nm=="exec")throw runtime_error("unsafe reduce");return vv(V::D);}
 P run(){while(p<b.size()){uint8_t c=g();switch(c){case 0x80:g();break;case 0x95:i8();break;case 'N':st.push_back(vv(V::N));break;case 0x88:st.push_back(iv(1));break;case 0x89:st.push_back(iv(0));break;case 'K':st.push_back(iv(g()));break;case 'M':st.push_back(iv(g()|(g()<<8)));break;case 'J':st.push_back(iv((int32_t)i4()));break;case 'G':i8();st.push_back(vv(V::N));break;case 'F':line();st.push_back(vv(V::N));break;case 'I':{auto x=line();st.push_back(iv((x=="01"||x=="True")?1:(x=="00"||x=="False")?0:stoll(x)));break;}case 'L':{auto x=line();if(x.back()=='L')x.pop_back();st.push_back(iv(stoll(x)));break;}case 0x8a:{int n=g();auto x=raw(n);long long z=0;for(int j=n-1;j>=0;j--)z=(z<<8)|(uint8_t)x[j];st.push_back(iv(z));break;}case 'X':st.push_back(sv(raw(i4())));break;case 0x8c:st.push_back(sv(raw(g())));break;case 'T':{auto x=vv(V::B);x->s=raw(i4());st.push_back(x);break;}case 'U':{auto x=vv(V::B);x->s=raw(g());st.push_back(x);break;}case 'B':{auto x=vv(V::B);x->s=raw(i4());st.push_back(x);break;}case 'C':{auto x=vv(V::B);x->s=raw(g());st.push_back(x);break;}case '(':st.push_back(vv(V::N));mk.push_back(st.size()-1);break;case ')':st.push_back(vv(V::T));break;case 't':{auto x=vv(V::T);x->a=marked();st.push_back(x);break;}case 0x85:{auto x=vv(V::T);x->a={pop()};st.push_back(x);break;}case 0x86:{P b=pop(),a=pop(); P x=vv(V::T);x->a={a,b};st.push_back(x);break;}case 0x87:{P c=pop(),b=pop(),a=pop(); P x=vv(V::T);x->a={a,b,c};st.push_back(x);break;}case ']':st.push_back(vv(V::L));break;case 'l':{auto x=vv(V::L);x->a=marked();st.push_back(x);break;}case 'a':st[st.size()-2]->a.push_back(pop());break;case 'e':{auto x=marked();st.back()->a.insert(st.back()->a.end(),x.begin(),x.end());break;}case '}':st.push_back(vv(V::D));break;case 'd':{auto x=vv(V::D); auto y=marked();for(U i=0;i+1<y.size();i+=2)x->d[ss(y[i])]=y[i+1];st.push_back(x);break;}case 's':{P v=pop(),k=pop();st.back()->d[ss(k)]=v;break;}case 'u':{auto y=marked();for(U i=0;i+1<y.size();i+=2)st.back()->d[ss(y[i])]=y[i+1];break;}case 'c':{auto x=vv(V::G);x->m=line();x->nm=line();st.push_back(x);break;}case 0x93:{P n=pop(),m=pop(),x=vv(V::G);x->m=ss(m);x->nm=ss(n);st.push_back(x);break;}case 'q':memo(g(),st.back());break;case 'r':memo(i4(),st.back());break;case 'p':memo(stoll(line()),st.back());break;case 0x94:memo(mem.size(),st.back());break;case 'h':st.push_back(mem.at(g()));break;case 'j':st.push_back(mem.at(i4()));break;case 'g':st.push_back(mem.at(stoll(line())));break;case '0':pop();break;case '2':st.push_back(st.back());break;case '1':while(st.back()->k!=V::N)pop();pop();break;case 'Q':{P id=pop(),x=vv(V::ST);if(id->k==V::T&&id->a.size()>2){x->dt=id->a[1]->k==V::G?id->a[1]->nm:ss(id->a[1]);x->key=ss(id->a[2]);}st.push_back(x);break;}case 'P':{auto x=vv(V::ST);x->key=line();st.push_back(x);break;}case 'R':{P ar=pop(),fn=pop();st.push_back(reduce(fn,ar));break;}case 'b':{P q=pop();if(st.back()->k==V::D&&q->k==V::D)for(auto&z:q->d)st.back()->d[z.first]=z.second;break;}case '.':return pop();default:throw runtime_error("opcode");}}throw runtime_error("stop");}
};
struct Tn{string p,dt,key;long long off;vector<long long>sh,st;};
void walk(P x,string path,vector<Tn>&o,int depth,unordered_set<V*>&active){
 if(!x)return;
 if(depth>2048)throw runtime_error("depth exceeded");
 auto r=active.insert(x.get()); if(!r.second)throw runtime_error("cycle");
 if(x->k==V::X)o.push_back({path,x->dt,x->key,x->off,x->sh,x->st});
 else if(x->k==V::D){for(auto&z:x->d)walk(z.second,path.empty()?z.first:path+"/"+z.first,o,depth+1,active);}
 else if(x->k==V::L||x->k==V::T){for(U i=0;i<x->a.size();i++)walk(x->a[i],path.empty()?to_string(i):path+"/"+to_string(i),o,depth+1,active);}
 active.erase(r.first);
}
void walk(P x,string path,vector<Tn>&o){unordered_set<V*>active;walk(x,path,o,0,active);}
string esc(string s){string r;for(char c:s){if(c=='"'||c=='\\')r+='\\';r+=c;}return r;}
void jv(P x,ostream& o){
 if(!x){o<<"null";return;}
 if(x->k==V::I){o<<x->n;return;}
 if(x->k==V::N){o<<"null";return;}
 if(x->k==V::S){o<<"\""<<esc(x->s)<<"\"";return;}
 if(x->k==V::L||x->k==V::T){o<<"[";for(U i=0;i<x->a.size();i++){if(i)o<<",";jv(x->a[i],o);}o<<"]";return;}
 if(x->k==V::D){o<<"{";U i=0;for(auto&z:x->d){if(i++)o<<",";o<<"\""<<esc(z.first)<<"\":";jv(z.second,o);}o<<"}";return;}
 o<<"null";
}
string dtype(string d){if(d=="FloatStorage")return "f32";if(d=="HalfStorage")return "f16";if(d=="LongStorage")return "i64";if(d=="IntStorage")return "i32";if(d=="DoubleStorage")return "f64";if(d=="BFloat16Storage")return "bf16";if(d=="ShortStorage")return "i16";if(d=="CharStorage")return "i8";if(d=="ByteStorage")return "u8";if(d=="BoolStorage")return "bool";return d;}
size_t bytes(string d){if(d=="f64"||d=="i64")return 8;if(d=="f32"||d=="i32")return 4;if(d=="f16"||d=="bf16"||d=="i16")return 2;return 1;}
uint32_t half32(uint16_t h){uint32_t sg=uint32_t(h&0x8000)<<16;uint32_t e=(h>>10)&31,m=h&1023;if(!e){if(!m)return sg;while(!(m&1024)){m<<=1;--e;}++e;m&=1023;}else if(e==31)return sg|0x7f800000|(m<<13);e=e+112;return sg|(e<<23)|(m<<13);}
int main(int ac,char**av){
 try{
  if(ac!=3)throw runtime_error("usage");
  Zip z(av[1]); string pn=z.pkl(); auto pb=z.get(pn); auto root=Pick(pb).run();
  bool rvc=root&&root->k==V::D&&root->d.count("weight")&&root->d.count("config")&&root->d["weight"]&&root->d["config"]&&(root->d["weight"]->k==V::D)&&(root->d["weight"]->d.count("enc_p.emb_phone.weight"))&&(root->d["config"]->k==V::L||root->d["config"]->k==V::T);
  P tensorRoot=rvc?root->d["weight"]:root;
  vector<Tn>ts; walk(tensorRoot,"",ts); for(auto&t:ts){t.dt=dtype(t.dt);if(rvc)t.dt="f32";}
  map<string,vector<uint8_t>>raw; string pre=pn.substr(0,pn.size()-8);
  for(auto&t:ts)if(!raw.count(t.key))raw[t.key]=z.get(pre+"data/"+t.key);
  fs::path out=av[2]; fs::create_directories(out/"tensors");
  ofstream m(out/"manifest.json");
  m<<"{\"nulltorch_manifest\":1,\"byteorder\":\"little\"";
  if(rvc){
   static const char* names[]={"spec_channels","segment_size","inter_channels","hidden_channels","filter_channels","n_heads","n_layers","kernel_size","p_dropout","resblock","resblock_kernel_sizes","resblock_dilation_sizes","upsample_rates","upsample_initial_channel","upsample_kernel_sizes","n_speakers","gin_channels","sr","phone_dim"};
   P cfg=root->d["config"]; m<<",\"config\":{";
   for(U i=0;i<19;i++){if(i)m<<",";m<<"\""<<names[i]<<"\":";if(cfg&&i<cfg->a.size()&&i==8&&cfg->a[i]->k==V::N)m<<"0.0";else if(cfg&&i<cfg->a.size()&&i>=15&&i<=18&&cfg->a[i]->k==V::S)m<<stoll(cfg->a[i]->s);else if(rvc&&i==18){long long ph=0;for(auto& t:ts)if(t.p=="enc_p.emb_phone.weight"&&t.sh.size()>1)ph=t.sh[1];m<<ph;}else if(cfg&&i<cfg->a.size())jv(cfg->a[i],m);else m<<"null";} m<<"}";
  }
  m<<",\"tensors\":{";
  for(U i=0;i<ts.size();i++){auto&t=ts[i];if(i)m<<',';U n=1;for(auto x:t.sh)n*=x;m<<'"'<<esc(t.p)<<'"'<<R"(:{"dtype":")"<<t.dt<<R"(","shape":[)";for(U j=0;j<t.sh.size();j++){if(j)m<<',';m<<t.sh[j];}m<<']';if(!rvc){m<<R"(,"stride":[)";for(U j=0;j<t.st.size();j++){if(j)m<<',';m<<t.st[j];}m<<R"(],"storage_key":")"<<t.key<<'"'<<R"(,"storage_offset":)"<<t.off;}m<<R"(,"nbytes":)"<<n*bytes(t.dt)<<'}';}m<<"}}";
  for(auto&t:ts){U n=1;for(auto x:t.sh)n*=x;size_t is=bytes(t.dt),srcIs=rvc?2:is;vector<uint8_t>o(n*is);for(U q=0;q<n;q++){U r=q,src=t.off;for(int j=(int)t.sh.size()-1;j>=0;j--){U ix=t.sh[j]?r%t.sh[j]:0;r=t.sh[j]?r/t.sh[j]:0;src+=ix*t.st[j];}U a=src*srcIs;if(a+srcIs<=raw[t.key].size()){if(rvc){uint16_t h=raw[t.key][a]|(uint16_t(raw[t.key][a+1])<<8);uint32_t bits=half32(h);for(int z=0;z<4;z++)o[q*4+z]=uint8_t(bits>>(8*z));}else copy(raw[t.key].begin()+a,raw[t.key].begin()+a+is,o.begin()+q*is);}}string fn=t.p;for(size_t k=0;(k=fn.find('/',k))!=string::npos;){fn.replace(k,1,"__");k+=2;}ofstream f(out/"tensors"/(fn+".bin"),ios::binary);f.write((char*)o.data(),o.size());}
  return 0;
 }catch(exception&e){cerr<<e.what()<<'\n';return 1;}
}
