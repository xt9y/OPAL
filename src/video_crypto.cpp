#include <opal/video_crypto.hpp>
#include <algorithm>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <string>

namespace opal {
namespace {
constexpr char kExporterLabel[]="EXPORTER-OPAL-DIRECT-VIDEO-v1";
constexpr std::size_t kTagBytes=16;

std::array<std::uint8_t,12> nonce_for(
    const std::array<std::uint8_t,12> &base,std::uint64_t sequence){
    auto nonce=base;
    for(int i=0;i<8;++i)
        nonce[11-i]^=static_cast<std::uint8_t>(sequence>>(i*8));
    return nonce;
}
}

bool derive_video_keys(SSL *ssl,std::string_view session_token,
                       std::string_view client_pub,std::string_view host_fp,
                       bool client_side,VideoKeys &keys){
    if(!ssl)return false;
    std::string context;
    context.reserve(session_token.size()+client_pub.size()+host_fp.size()+2);
    context.append(session_token);
    context.push_back('\n');
    context.append(client_pub);
    context.push_back('\n');
    context.append(host_fp);

    std::array<std::uint8_t,88> material{};
    if(SSL_export_keying_material(
        ssl,material.data(),material.size(),kExporterLabel,sizeof(kExporterLabel)-1,
        reinterpret_cast<const unsigned char*>(context.data()),context.size(),1)!=1)
        return false;

    auto copy32=[&](std::array<std::uint8_t,32> &dst,std::size_t offset){
        std::copy_n(material.begin()+static_cast<std::ptrdiff_t>(offset),32,dst.begin());
    };
    auto copy12=[&](std::array<std::uint8_t,12> &dst,std::size_t offset){
        std::copy_n(material.begin()+static_cast<std::ptrdiff_t>(offset),12,dst.begin());
    };

    if(client_side){
        copy32(keys.send_key,0);
        copy32(keys.recv_key,32);
        copy12(keys.send_nonce_base,64);
        copy12(keys.recv_nonce_base,76);
    }else{
        copy32(keys.recv_key,0);
        copy32(keys.send_key,32);
        copy12(keys.recv_nonce_base,64);
        copy12(keys.send_nonce_base,76);
    }
    OPENSSL_cleanse(material.data(),material.size());
    return true;
}

struct VideoCipher::Impl {
    explicit Impl(const VideoKeys &input):keys(input){
        seal_ctx=EVP_CIPHER_CTX_new();
        open_ctx=EVP_CIPHER_CTX_new();
    }
    ~Impl(){
        EVP_CIPHER_CTX_free(seal_ctx);
        EVP_CIPHER_CTX_free(open_ctx);
        OPENSSL_cleanse(&keys,sizeof(keys));
    }
    VideoKeys keys;
    EVP_CIPHER_CTX *seal_ctx=nullptr;
    EVP_CIPHER_CTX *open_ctx=nullptr;
};

VideoCipher::VideoCipher(const VideoKeys &keys):impl_(std::make_unique<Impl>(keys)){}
VideoCipher::~VideoCipher()=default;

bool VideoCipher::valid() const{return impl_&&impl_->seal_ctx&&impl_->open_ctx;}

bool VideoCipher::seal(std::uint64_t sequence,std::span<const std::uint8_t> aad,
                       std::span<const std::uint8_t> plaintext,std::span<std::uint8_t> output,
                       std::size_t &output_size){
    output_size=0;
    if(!valid()||output.size()<plaintext.size()+kTagBytes)return false;
    auto *ctx=impl_->seal_ctx;
    if(EVP_CIPHER_CTX_reset(ctx)!=1)return false;
    const auto nonce=nonce_for(impl_->keys.send_nonce_base,sequence);
    int written=0,total=0;
    if(EVP_EncryptInit_ex(ctx,EVP_chacha20_poly1305(),nullptr,nullptr,nullptr)!=1)return false;
    if(EVP_CIPHER_CTX_ctrl(ctx,EVP_CTRL_AEAD_SET_IVLEN,static_cast<int>(nonce.size()),nullptr)!=1)return false;
    if(EVP_EncryptInit_ex(ctx,nullptr,nullptr,impl_->keys.send_key.data(),nonce.data())!=1)return false;
    if(!aad.empty()&&EVP_EncryptUpdate(ctx,nullptr,&written,aad.data(),static_cast<int>(aad.size()))!=1)return false;
    if(!plaintext.empty()&&EVP_EncryptUpdate(ctx,output.data(),&written,plaintext.data(),static_cast<int>(plaintext.size()))!=1)return false;
    total=written;
    if(EVP_EncryptFinal_ex(ctx,output.data()+total,&written)!=1)return false;
    total+=written;
    if(EVP_CIPHER_CTX_ctrl(ctx,EVP_CTRL_AEAD_GET_TAG,static_cast<int>(kTagBytes),output.data()+total)!=1)return false;
    output_size=static_cast<std::size_t>(total)+kTagBytes;
    return output_size<=output.size();
}

bool VideoCipher::open(std::uint64_t sequence,std::span<const std::uint8_t> aad,
                       std::span<const std::uint8_t> ciphertext_tag,std::span<std::uint8_t> output,
                       std::size_t &output_size){
    output_size=0;
    if(!valid()||ciphertext_tag.size()<kTagBytes)return false;
    const std::size_t ciphertext_size=ciphertext_tag.size()-kTagBytes;
    if(output.size()<ciphertext_size)return false;
    auto *ctx=impl_->open_ctx;
    if(EVP_CIPHER_CTX_reset(ctx)!=1)return false;
    const auto nonce=nonce_for(impl_->keys.recv_nonce_base,sequence);
    int written=0,total=0;
    if(EVP_DecryptInit_ex(ctx,EVP_chacha20_poly1305(),nullptr,nullptr,nullptr)!=1)return false;
    if(EVP_CIPHER_CTX_ctrl(ctx,EVP_CTRL_AEAD_SET_IVLEN,static_cast<int>(nonce.size()),nullptr)!=1)return false;
    if(EVP_DecryptInit_ex(ctx,nullptr,nullptr,impl_->keys.recv_key.data(),nonce.data())!=1)return false;
    if(!aad.empty()&&EVP_DecryptUpdate(ctx,nullptr,&written,aad.data(),static_cast<int>(aad.size()))!=1)return false;
    if(ciphertext_size&&EVP_DecryptUpdate(ctx,output.data(),&written,ciphertext_tag.data(),static_cast<int>(ciphertext_size))!=1)return false;
    total=written;
    if(EVP_CIPHER_CTX_ctrl(ctx,EVP_CTRL_AEAD_SET_TAG,static_cast<int>(kTagBytes),
                           const_cast<std::uint8_t*>(ciphertext_tag.data()+ciphertext_size))!=1)return false;
    if(EVP_DecryptFinal_ex(ctx,output.data()+total,&written)!=1)return false;
    total+=written;
    output_size=static_cast<std::size_t>(total);
    return output_size<=output.size();
}

bool seal_video_datagram(const VideoKeys &keys,std::uint64_t sequence,
                         std::span<const std::uint8_t> aad,
                         std::span<const std::uint8_t> plaintext,
                         std::vector<std::uint8_t> &output){
    output.resize(plaintext.size()+kTagBytes);
    VideoCipher cipher(keys);std::size_t size=0;
    if(!cipher.valid()||!cipher.seal(sequence,aad,plaintext,output,size)){output.clear();return false;}
    output.resize(size);return true;
}

bool open_video_datagram(const VideoKeys &keys,std::uint64_t sequence,
                         std::span<const std::uint8_t> aad,
                         std::span<const std::uint8_t> ciphertext_tag,
                         std::vector<std::uint8_t> &output){
    if(ciphertext_tag.size()<kTagBytes){output.clear();return false;}
    output.resize(ciphertext_tag.size()-kTagBytes);
    VideoCipher cipher(keys);std::size_t size=0;
    if(!cipher.valid()||!cipher.open(sequence,aad,ciphertext_tag,output,size)){output.clear();return false;}
    output.resize(size);return true;
}

bool ReplayWindow1024::accept(std::uint64_t sequence){
    if(!initialized_){
        initialized_=true;
        highest_=sequence;
        seen_.reset();
        seen_.set(0);
        return true;
    }
    if(sequence>highest_){
        const std::uint64_t advance=sequence-highest_;
        if(advance>=1024)seen_.reset();
        else seen_<<=static_cast<std::size_t>(advance);
        highest_=sequence;
        seen_.set(0);
        return true;
    }
    const std::uint64_t age=highest_-sequence;
    if(age>=1024)return false;
    const auto bit=static_cast<std::size_t>(age);
    if(seen_.test(bit))return false;
    seen_.set(bit);
    return true;
}

void ReplayWindow1024::reset(){
    initialized_=false;
    highest_=0;
    seen_.reset();
}
}
