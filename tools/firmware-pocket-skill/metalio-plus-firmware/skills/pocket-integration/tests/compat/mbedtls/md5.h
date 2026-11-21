#pragma once
#include "sha256.h"
inline int mbedtls_md5(const unsigned char* p,size_t n,unsigned char* out){
    BCRYPT_ALG_HANDLE a=nullptr;BCRYPT_HASH_HANDLE h=nullptr;
    bool ok=!BCryptOpenAlgorithmProvider(&a,BCRYPT_MD5_ALGORITHM,nullptr,0)&&
        !BCryptCreateHash(a,&h,nullptr,0,nullptr,0,0)&&!BCryptHashData(h,const_cast<PUCHAR>(p),ULONG(n),0)&&
        !BCryptFinishHash(h,out,16,0);
    if(h)BCryptDestroyHash(h);if(a)BCryptCloseAlgorithmProvider(a,0);return ok?0:-1;
}
