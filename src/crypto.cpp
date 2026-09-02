#include <opal/crypto.hpp>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <sys/stat.h>
#include <fstream>
#include <stdexcept>

namespace opal {
std::string hex(const unsigned char*p,size_t n){static const char*d="0123456789abcdef";std::string s; s.reserve(n*2);for(size_t i=0;i<n;i++){s+=d[p[i]>>4];s+=d[p[i]&15];}return s;}
std::vector<unsigned char> unhex(const std::string&s){if(s.size()%2) return {};std::vector<unsigned char>v(s.size()/2);auto cv=[](char c)->int{if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;};for(size_t i=0;i<v.size();++i){int a=cv(s[2*i]),b=cv(s[2*i+1]);if(a<0||b<0)return{};v[i]=static_cast<unsigned char>((a<<4)|b);}return v;}
std::string random_hex(size_t bytes){std::vector<unsigned char>b(bytes);if(RAND_bytes(b.data(),static_cast<int>(b.size()))!=1) throw std::runtime_error("RAND_bytes failed");return hex(b.data(),b.size());}
std::string pairing_code(){static const char*a="ABCDEFGHJKLMNPQRSTUVWXYZ23456789";unsigned char r[8];RAND_bytes(r,8);std::string s;for(int i=0;i<8;i++){if(i==4)s+='-';s+=a[r[i]%32];}return s;}
std::string hmac_sha256_hex(const std::string&key,const std::string&data){unsigned char out[EVP_MAX_MD_SIZE];unsigned int n=0;HMAC(EVP_sha256(),key.data(),static_cast<int>(key.size()),reinterpret_cast<const unsigned char*>(data.data()),data.size(),out,&n);return hex(out,n);}
bool secure_equal(const std::string&a,const std::string&b){if(a.size()!=b.size())return false;unsigned char x=0;for(size_t i=0;i<a.size();i++)x|=static_cast<unsigned char>(a[i]^b[i]);return x==0;}
bool ensure_identity(const std::filesystem::path&priv,const std::filesystem::path&pub){if(std::filesystem::exists(priv)&&std::filesystem::exists(pub))return true;EVP_PKEY_CTX*ctx=EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519,nullptr);if(!ctx)return false;EVP_PKEY*p=nullptr;bool ok=EVP_PKEY_keygen_init(ctx)==1&&EVP_PKEY_keygen(ctx,&p)==1;EVP_PKEY_CTX_free(ctx);if(!ok)return false;size_t pn=32,qn=32;unsigned char pr[32],pu[32];ok=EVP_PKEY_get_raw_private_key(p,pr,&pn)==1&&EVP_PKEY_get_raw_public_key(p,pu,&qn)==1;EVP_PKEY_free(p);if(!ok)return false;std::ofstream f(priv,std::ios::binary|std::ios::trunc),g(pub,std::ios::binary|std::ios::trunc);f.write(reinterpret_cast<char*>(pr),pn);g.write(reinterpret_cast<char*>(pu),qn);f.close();g.close();chmod(priv.c_str(),0600);chmod(pub.c_str(),0644);return f.good()&&g.good();}
static std::vector<unsigned char> readbin(const std::filesystem::path&p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
std::string public_key_hex(const std::filesystem::path&pub){auto v=readbin(pub);return v.size()==32?hex(v.data(),v.size()):std::string();}
std::string sign_hex(const std::filesystem::path&priv,const std::string&m){auto k=readbin(priv);if(k.size()!=32)return{};EVP_PKEY*p=EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519,nullptr,k.data(),k.size());EVP_MD_CTX*c=EVP_MD_CTX_new();size_t n=64;unsigned char sig[64];bool ok=p&&c&&EVP_DigestSignInit(c,nullptr,nullptr,nullptr,p)==1&&EVP_DigestSign(c,sig,&n,reinterpret_cast<const unsigned char*>(m.data()),m.size())==1;EVP_MD_CTX_free(c);EVP_PKEY_free(p);return ok?hex(sig,n):std::string();}
bool verify_hex(const std::string&ph,const std::string&m,const std::string&sh){auto p=unhex(ph),s=unhex(sh);if(p.size()!=32||s.size()!=64)return false;EVP_PKEY*k=EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519,nullptr,p.data(),p.size());EVP_MD_CTX*c=EVP_MD_CTX_new();bool ok=k&&c&&EVP_DigestVerifyInit(c,nullptr,nullptr,nullptr,k)==1&&EVP_DigestVerify(c,s.data(),s.size(),reinterpret_cast<const unsigned char*>(m.data()),m.size())==1;EVP_MD_CTX_free(c);EVP_PKEY_free(k);return ok;}
}
