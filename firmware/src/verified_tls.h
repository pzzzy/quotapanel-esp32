// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <WiFiClientSecure.h>
#include <time.h>
#include "recovery_policy.h"

// This pinned Arduino SDK was built without MBEDTLS_HAVE_TIME_DATE. Re-run
// standard chain/hostname verification with a date callback before connect()
// succeeds, so HTTPClient cannot transmit headers or credentials first.
class VerifiedTLSClient : public WiFiClientSecure {
    static recovery::Calendar calendar(const mbedtls_x509_time& t) {
        return {t.year,t.mon,t.day,t.hour,t.min,t.sec};
    }
    static int checkTime(void* context,mbedtls_x509_crt* certificate,int,uint32_t* flags) {
        auto now=static_cast<const recovery::Calendar*>(context);
        int validity=recovery::certificateTime(calendar(certificate->valid_from),calendar(certificate->valid_to),*now);
        if(validity<0)*flags|=MBEDTLS_X509_BADCERT_FUTURE;
        if(validity>0)*flags|=MBEDTLS_X509_BADCERT_EXPIRED;
        return 0; // Keep all existing flags; the verifier rejects any failure.
    }
public:
    uint32_t verificationFlags=0;
    int connect(const char* host,uint16_t port) override {
        verificationFlags=0;
        if(!WiFiClientSecure::connect(host,port))return 0;
        time_t epoch=time(nullptr);struct tm utc;
        auto peer=getPeerCertificate();
        if(!peer||epoch<1760000000||!gmtime_r(&epoch,&utc)){
            stop();_lastError=MBEDTLS_ERR_X509_CERT_VERIFY_FAILED;return 0;
        }
        recovery::Calendar now{utc.tm_year+1900,utc.tm_mon+1,utc.tm_mday,utc.tm_hour,utc.tm_min,utc.tm_sec};
        // Arduino frees its CA chain after the handshake. Use a fresh parsed
        // trust store for this second verification; never verify with an empty
        // or previously freed chain.
        mbedtls_x509_crt roots;mbedtls_x509_crt_init(&roots);
        int parsed=_CA_cert?mbedtls_x509_crt_parse(&roots,
            reinterpret_cast<const unsigned char*>(_CA_cert),strlen(_CA_cert)+1):-1;
        int verified=parsed<0?parsed:mbedtls_x509_crt_verify(const_cast<mbedtls_x509_crt*>(peer),
            &roots,nullptr,host,&verificationFlags,checkTime,&now);
        mbedtls_x509_crt_free(&roots);
        if(verified!=0||verificationFlags){stop();_lastError=MBEDTLS_ERR_X509_CERT_VERIFY_FAILED;return 0;}
        return 1;
    }
    int connect(const char* host,uint16_t port,int32_t timeout) override {
        _timeout=timeout;return connect(host,port);
    }
    // Account requests always use a hostname, never an unverified bare address.
    int connect(IPAddress,uint16_t) override {_lastError=MBEDTLS_ERR_X509_BAD_INPUT_DATA;return 0;}
    int connect(IPAddress,uint16_t,int32_t) override {_lastError=MBEDTLS_ERR_X509_BAD_INPUT_DATA;return 0;}
};
