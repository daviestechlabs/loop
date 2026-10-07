#include "cmp_gateway_identity.h"
#include "cmp_json.h"
#include "cmp_oauth.h"
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/core_names.h>
#include <openssl/param_build.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int unique(const cmp_json_object *o) {
    for (size_t i=0;i<o->field_count;i++) {
        if (o->fields[i].key_escaped) return 0;
        for (size_t j=0;j<i;j++) if (o->fields[i].key_len==o->fields[j].key_len &&
            !memcmp(o->fields[i].key,o->fields[j].key,o->fields[i].key_len)) return 0;
    }
    return 1;
}
static size_t decode(const char *s,size_t len,unsigned char *out,size_t cap) {
    unsigned bits=0,value=0; size_t n=0;
    if (!len || len%4==1) return 0;
    for (size_t i=0;i<len;i++) {
        unsigned char c=(unsigned char)s[i]; unsigned digit;
        if (c>='A' && c<='Z') digit=c-'A'; else if (c>='a' && c<='z') digit=c-'a'+26;
        else if (c>='0' && c<='9') digit=c-'0'+52; else if (c=='-') digit=62; else if (c=='_') digit=63; else return 0;
        value=(value<<6)|digit; bits+=6;
        if (bits>=8) { bits-=8; if (n==cap) return 0; out[n++]=(unsigned char)(value>>bits); }
    }
    return bits && (value&((1u<<bits)-1u)) ? 0 : n;
}
static int contains(const cmp_json_field *field,const char *wanted,int scalar,int *count) {
    char text[1024]; *count=0;
    if (scalar && cmp_json_field_str(field,text,sizeof(text))) { *count=1; return !strcmp(text,wanted); }
    cmp_json_array array; cmp_json_field item; int rc,found=0;
    if (!cmp_json_field_array(field,&array)) return 0;
    while ((rc=cmp_json_array_next(&array,&item))==1) {
        if (++*count>128 || !cmp_json_field_str(&item,text,sizeof(text))) return 0;
        if (!strcmp(text,wanted)) found=1;
    }
    return rc==0 && found;
}
static EVP_PKEY *rsa_key(const cmp_json_object *o) {
    char n[1400],e[16],text[40]; unsigned char modulus[1024],exponent[8]; int count;
    if (!cmp_json_object_str(o,"kty",text,sizeof(text)) || strcmp(text,"RSA") ||
        (cmp_json_object_field(o,"alg") && (!cmp_json_object_str(o,"alg",text,sizeof(text)) || strcmp(text,"RS256"))) ||
        (cmp_json_object_field(o,"use") && (!cmp_json_object_str(o,"use",text,sizeof(text)) || strcmp(text,"sig"))) ||
        (cmp_json_object_field(o,"key_ops") && !contains(cmp_json_object_field(o,"key_ops"),"verify",0,&count)) ||
        !cmp_json_object_str(o,"n",n,sizeof(n)) || !cmp_json_object_str(o,"e",e,sizeof(e))) return NULL;
    size_t nl=decode(n,strlen(n),modulus,sizeof(modulus)),el=decode(e,strlen(e),exponent,sizeof(exponent));
    if (nl<256 || !el || el>4 || modulus[0]==0 || exponent[0]==0) return NULL;
    BIGNUM *bn=BN_bin2bn(modulus,(int)nl,NULL),*be=BN_bin2bn(exponent,(int)el,NULL);
    EVP_PKEY *key=NULL; EVP_PKEY_CTX *ctx=NULL; OSSL_PARAM_BLD *builder=NULL; OSSL_PARAM *params=NULL;
    if (!bn || !be || BN_num_bits(bn)<2048 || !BN_is_odd(bn) || !BN_is_odd(be) || BN_is_one(be)) goto done;
    builder=OSSL_PARAM_BLD_new(); ctx=EVP_PKEY_CTX_new_from_name(NULL,"RSA",NULL);
    if (!builder || !ctx || !OSSL_PARAM_BLD_push_BN(builder,OSSL_PKEY_PARAM_RSA_N,bn) ||
        !OSSL_PARAM_BLD_push_BN(builder,OSSL_PKEY_PARAM_RSA_E,be) || !(params=OSSL_PARAM_BLD_to_param(builder)) ||
        EVP_PKEY_fromdata_init(ctx)<=0 || EVP_PKEY_fromdata(ctx,&key,EVP_PKEY_PUBLIC_KEY,params)<=0) {
        EVP_PKEY_free(key); key=NULL;
    }
done:
    OSSL_PARAM_free(params); OSSL_PARAM_BLD_free(builder); EVP_PKEY_CTX_free(ctx); BN_free(bn); BN_free(be); return key;
}
/* 404 is internal: an otherwise parseable token names an uncached signing key. */
static int verify(const char *token,const char *jwks,const cmp_gateway_policy *p,int64_t now,char subject[256]) {
    subject[0]=0;
    if (!token || !jwks || !p || !p->issuer || !p->audience || !p->required_group || now<0 || now>253402300799LL ||
        strlen(token)>CMP_GATEWAY_TOKEN_CAP || strlen(jwks)>CMP_GATEWAY_JWKS_CAP) return 401;
    const char *a=strchr(token,'.'),*b=a?strchr(a+1,'.'):NULL;
    if (!a || !b || strchr(b+1,'.')) return 401;
    char header[2049],claims[CMP_GATEWAY_TOKEN_CAP+1],kid[256],alg[32],issuer[1024],sub[256],azp[512];
    unsigned char signature[1024];
    size_t hn=decode(token,(size_t)(a-token),(unsigned char *)header,sizeof(header)-1);
    size_t cn=decode(a+1,(size_t)(b-a-1),(unsigned char *)claims,sizeof(claims)-1);
    size_t sn=decode(b+1,strlen(b+1),signature,sizeof(signature));
    if (!hn || !cn || !sn || memchr(header,0,hn) || memchr(claims,0,cn)) return 401;
    header[hn]=0; claims[cn]=0;
    cmp_json_object h,c,keys; cmp_json_array array; cmp_json_field item;
    int64_t issued,expires,nbf; int audiences=0;
    if (!cmp_json_object_parse(header,&h) || !unique(&h) || !cmp_json_object_parse(claims,&c) || !unique(&c) ||
        !cmp_json_object_str(&h,"alg",alg,sizeof(alg)) || strcmp(alg,"RS256") ||
        !cmp_json_object_str(&h,"kid",kid,sizeof(kid)) || !*kid ||
        cmp_json_object_field(&h,"crit") || cmp_json_object_field(&h,"b64") || cmp_json_object_field(&h,"jku") ||
        cmp_json_object_field(&h,"jwk") || cmp_json_object_field(&h,"x5u") ||
        (cmp_json_object_field(&h,"typ") && (!cmp_json_object_str(&h,"typ",alg,sizeof(alg)) || strcmp(alg,"JWT"))) ||
        !cmp_json_object_str(&c,"iss",issuer,sizeof(issuer)) || strcmp(issuer,p->issuer) ||
        !contains(cmp_json_object_field(&c,"aud"),p->audience,1,&audiences) ||
        ((audiences>1 || cmp_json_object_field(&c,"azp")) && (!cmp_json_object_str(&c,"azp",azp,sizeof(azp)) || strcmp(azp,p->audience))) ||
        !cmp_json_object_str(&c,"sub",sub,sizeof(sub)) || !*sub || strspn(sub," \t\r\n")==strlen(sub) ||
        !cmp_json_object_i64(&c,"iat",&issued) || !cmp_json_object_i64(&c,"exp",&expires) ||
        issued<0 || expires<0 || expires>253402300799LL || issued>now+5 || now-issued>3605 || expires<=issued || expires<=now-5 ||
        (cmp_json_object_field(&c,"nbf") && (!cmp_json_object_i64(&c,"nbf",&nbf) || nbf<0 || nbf>now+5))) return 401;
    if (!cmp_json_object_parse(jwks,&keys) || !unique(&keys) || !cmp_json_field_array(cmp_json_object_field(&keys,"keys"),&array)) return 503;
    EVP_PKEY *key=NULL; int rc,matched=0,key_count=0;
    while ((rc=cmp_json_array_next(&array,&item))==1) {
        cmp_json_object k; char id[256];
        if (++key_count>32 || !cmp_json_field_object(&item,&k) || !unique(&k) || !cmp_json_object_str(&k,"kid",id,sizeof(id))) { EVP_PKEY_free(key); return 503; }
        if (!strcmp(id,kid)) { matched++; if (matched==1) key=rsa_key(&k); }
    }
    if (rc<0 || matched>1) { EVP_PKEY_free(key); return 503; }
    if (!matched) return 404;
    if (!key) return 401;
    EVP_MD_CTX *md=EVP_MD_CTX_new(); EVP_PKEY_CTX *pk=NULL;
    int valid=md && EVP_DigestVerifyInit(md,&pk,EVP_sha256(),NULL,key)==1 && EVP_PKEY_CTX_set_rsa_padding(pk,RSA_PKCS1_PADDING)>0 &&
        EVP_DigestVerify(md,signature,sn,(const unsigned char *)token,(size_t)(b-token))==1;
    EVP_MD_CTX_free(md); EVP_PKEY_free(key);
    if (!valid) return 401;
    int groups=0;
    if (!contains(cmp_json_object_field(&c,"groups"),p->required_group,0,&groups)) return 403;
    memcpy(subject,sub,strlen(sub)+1); return 200;
}
int cmp_gateway_verify(const char *token,const char *jwks,const cmp_gateway_policy *p,int64_t now,char subject[256]) {
    int result=verify(token,jwks,p,now,subject); return result==404?401:result;
}
int cmp_gateway_identity_init(cmp_gateway_identity *id,const cmp_gateway_policy *p) {
    if (!id || !p || !p->issuer || strncmp(p->issuer,"https://",8) || !p->audience || !*p->audience ||
        !p->required_group || !*p->required_group || !p->jwks_url || strncmp(p->jwks_url,"https://",8)) return 0;
    memset(id,0,sizeof(*id)); id->policy=*p; id->attempted=-30;
    if (pthread_mutex_init(&id->cache_mutex,NULL)) return 0;
    if (pthread_mutex_init(&id->refresh_mutex,NULL)) { pthread_mutex_destroy(&id->cache_mutex); return 0; }
    return 1;
}
void cmp_gateway_identity_close(cmp_gateway_identity *id) {
    pthread_mutex_destroy(&id->refresh_mutex); pthread_mutex_destroy(&id->cache_mutex);
}
int cmp_gateway_identity_verify(cmp_gateway_identity *id,const char *token,char subject[256]) {
    struct timespec clock; subject[0]=0;
    if (clock_gettime(CLOCK_MONOTONIC,&clock)) return 503;
    int64_t now=clock.tv_sec; char *keys=malloc(CMP_GATEWAY_JWKS_CAP+1); if (!keys) return 503;
    pthread_mutex_lock(&id->cache_mutex);
    memcpy(keys,id->keys,sizeof(id->keys)); int fresh=now<id->expires;
    pthread_mutex_unlock(&id->cache_mutex);
    /* Validate the token before attempting a network refresh, even on a cold cache. */
    int result=verify(token,fresh?keys:"{\"keys\":[]}",&id->policy,(int64_t)time(NULL),subject);
    if (result!=404) { free(keys); return result; }
    if (pthread_mutex_trylock(&id->refresh_mutex)) { free(keys); return 503; }
    pthread_mutex_lock(&id->cache_mutex);
    int allowed=now-id->attempted>=30; if (allowed) id->attempted=now;
    pthread_mutex_unlock(&id->cache_mutex);
    if (!allowed) result=fresh?401:503;
    else {
        size_t length=0;
        if (cmp_oauth_public_keys_get(id->policy.jwks_url,(unsigned char *)keys,CMP_GATEWAY_JWKS_CAP,&length) ||
            !length || length>CMP_GATEWAY_JWKS_CAP || memchr(keys,0,length)) result=503;
        else {
            keys[length]=0;
            result=verify(token,keys,&id->policy,(int64_t)time(NULL),subject);
            if (result!=503) {
                pthread_mutex_lock(&id->cache_mutex);
                memcpy(id->keys,keys,length+1); id->expires=now+300;
                pthread_mutex_unlock(&id->cache_mutex);
            }
        }
    }
    pthread_mutex_unlock(&id->refresh_mutex); free(keys); return result==404?401:result;
}
