/* Copyright (C) 2018-2020 Intel Labs
   This file is part of Graphene Library OS.
   Graphene Library OS is free software: you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public License
   as published by the Free Software Foundation, either version 3 of the
   License, or (at your option) any later version.
   Graphene Library OS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU Lesser General Public License for more details.
   You should have received a copy of the GNU Lesser General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

/*!
 * \file
 *
 * This file contains the implementation of server-side attestation for TLS libraries. It contains
 * functions to create a self-signed RA-TLS certificate with an SGX quote embedded in it.
 *
 * This file is part of the RA-TLS attestation library which is typically linked into server
 * applications.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <mbedtls/sha256.h>
#include <mbedtls/x509_crt.h>

#include "ra_tls.h"
#include "attestation.h"
#include "ias.h"
#include "util.h"

#define CERT_SUBJECT_NAME_VALUES  "cn=RA-TLS,O=Graphene Developers,C=US"
#define CERT_TIMESTAMP_NOT_BEFORE "20010101000000"
#define CERT_TIMESTAMP_NOT_AFTER  "20301231235959"

/*! given public key \p pk, generate an RA-TLS certificate \p writecrt with \p quote embedded */
static int generate_x509(mbedtls_pk_context* pk, const uint8_t* quote, size_t quote_size,
                         mbedtls_x509write_cert* writecrt) {
    int ret;

    mbedtls_mpi serial;
    mbedtls_mpi_init(&serial);

    mbedtls_x509write_crt_init(writecrt);
    mbedtls_x509write_crt_set_md_alg(writecrt, MBEDTLS_MD_SHA256);

    /* generated certificate is self-signed, so declares itself both a subject and an issuer */
    mbedtls_x509write_crt_set_subject_key(writecrt, pk);
    mbedtls_x509write_crt_set_issuer_key(writecrt, pk);

    /* set (dummy) subject names for both subject and issuer */
    ret = mbedtls_x509write_crt_set_subject_name(writecrt, CERT_SUBJECT_NAME_VALUES);
    if (ret < 0) {
        ret = -1;
        goto out;
    }

    ret = mbedtls_x509write_crt_set_issuer_name(writecrt, CERT_SUBJECT_NAME_VALUES);
    if (ret < 0) {
        ret = -1;
        goto out;
    }

    /* set a serial number (dummy "1") for the generated certificate */
    ret = mbedtls_mpi_read_string(&serial, 10, "1");
    if (ret < 0) {
        ret = -1;
        goto out;
    }

    ret = mbedtls_x509write_crt_set_serial(writecrt, &serial);
    if (ret < 0) {
        ret = -1;
        goto out;
    }

    char* cert_timestamp_not_before = getenv(RA_TLS_CERT_TIMESTAMP_NOT_BEFORE);
    if (!cert_timestamp_not_before)
        cert_timestamp_not_before = CERT_TIMESTAMP_NOT_BEFORE;

    char* cert_timestamp_not_after = getenv(RA_TLS_CERT_TIMESTAMP_NOT_AFTER);
    if (!cert_timestamp_not_after)
        cert_timestamp_not_after = CERT_TIMESTAMP_NOT_AFTER;

    ret = mbedtls_x509write_crt_set_validity(writecrt, cert_timestamp_not_before,
                                             cert_timestamp_not_after);
    if (ret < 0) {
        ret = -1;
        goto out;
    }

    ret = mbedtls_x509write_crt_set_basic_constraints(writecrt, /*is_ca=*/0, /*max_pathlen=*/-1);
    if (ret < 0) {
        ret = -1;
        goto out;
    }

    ret = mbedtls_x509write_crt_set_subject_key_identifier(writecrt);
    if (ret < 0) {
        ret = -1;
        goto out;
    }

    ret = mbedtls_x509write_crt_set_authority_key_identifier(writecrt);
    if (ret < 0) {
        ret = -1;
        goto out;
    }

    /* finally, embed the quote into the generated certificate (as X.509 extension) */
    ret = mbedtls_x509write_crt_set_extension(writecrt, (const char*)quote_oid, quote_oid_len,
                                              /*critical=*/0, quote, quote_size);
    if (ret < 0) {
        ret = -1;
        goto out;
    }

    ret = 0;
out:
    mbedtls_mpi_free(&serial);
    return ret;
}

/*! calculate sha256 over public key \p pk and copy it into \p sha */
static int sha256_over_pk(mbedtls_pk_context* pk, uint8_t* sha) {
    uint8_t pk_der[PUB_KEY_SIZE_MAX] = {0};

    /* below function writes data at the end of the buffer */
    int pk_der_size_byte = mbedtls_pk_write_pubkey_der(pk, pk_der, PUB_KEY_SIZE_MAX);
    if (pk_der_size_byte != RSA_PUB_3072_KEY_DER_LEN)
        return -1;

    /* move the data to the beginning of the buffer, to avoid pointer arithmetic later */
    memmove(pk_der, pk_der + PUB_KEY_SIZE_MAX - pk_der_size_byte, pk_der_size_byte);

    int ret = mbedtls_sha256_ret(pk_der, pk_der_size_byte, sha, /*is224=*/0);
    if (ret)
        return -1;

    return 0;
}

/*! given public key \p pk, generate an RA-TLS certificate \p writecrt */
static int create_x509(mbedtls_pk_context* pk, mbedtls_x509write_cert* writecrt) {
    int ret;
    uint8_t* quote_blob = NULL;

    sgx_report_data_t report_data = {0};
    ret = sha256_over_pk(pk, report_data.d);
    if (ret < 0) {
        ret = -1;
        goto out;
    }

    ret = write_file("/dev/attestation/report_data", sizeof(report_data), &report_data);
    if (ret < 0) {
        ret = -1;
        goto out;
    }

    ssize_t quote_size = 0;
    quote_blob = read_file("/dev/attestation/quote", &quote_size);
    if (!quote_blob) {
        ERROR("Failed to read quote file\n");
        return -1;
    }

    ret = generate_x509(pk, quote_blob, quote_size, writecrt);
    if (ret < 0) {
        ret = -1;
        goto out;
    }

    ret = 0;
out:
    free(quote_blob);
    return ret;
}

static int create_key_and_crt(mbedtls_pk_context* key, mbedtls_x509_crt* crt,
                              uint8_t* crt_der, size_t* crt_der_size) {
    int ret;

    if (!key || !crt)
        return -1;

    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_ctr_drbg_init(&ctr_drbg);

    mbedtls_entropy_context entropy;
    mbedtls_entropy_init(&entropy);

    mbedtls_x509write_cert writecrt;
    mbedtls_x509write_crt_init(&writecrt);

    uint8_t* output_buf    = NULL;
    size_t output_buf_size = 16 * 1024;  /* enough for any X.509 certificate */

    output_buf = malloc(output_buf_size);
    if (!output_buf) {
        ret = -1;
        goto out;
    }

    ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy, /*custom=*/NULL,
                                /*customlen=*/0);
    if (ret < 0) {
        ret = -1;
        goto out;
    }

    ret = mbedtls_pk_setup(key, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA));
    if (ret < 0) {
        ret = -1;
        goto out;
    }

    mbedtls_rsa_init((mbedtls_rsa_context*)key->pk_ctx, MBEDTLS_RSA_PKCS_V15, /*hash_id=*/0);

    ret = mbedtls_rsa_gen_key((mbedtls_rsa_context*)key->pk_ctx, mbedtls_ctr_drbg_random,
                              &ctr_drbg, RSA_PUB_3072_KEY_LEN, RSA_PUB_EXPONENT);
    if (ret < 0) {
        ret = -1;
        goto out;
    }

    ret = create_x509(key, &writecrt);
    if (ret < 0) {
        ret = -1;
        goto out;
    }

    int size = mbedtls_x509write_crt_der(&writecrt, output_buf, output_buf_size,
                                         mbedtls_ctr_drbg_random, &ctr_drbg);
    if (size < 0) {
        ret = -1;
        goto out;
    }

    /* note that previous function wrote data at the end of the output_buf */
    if (crt_der && crt_der_size) {
        if (*crt_der_size >= size) {
            /* populate crt_der only if user provided sufficiently big buffer */
            memcpy(crt_der, output_buf + output_buf_size - size, size);
        }
        *crt_der_size = size;
    }

    if (crt) {
        ret = mbedtls_x509_crt_parse_der(crt, output_buf + output_buf_size - size, size);
        if (ret < 0) {
            ret = -1;
            goto out;
        }
    }

    ret = 0;
out:
    mbedtls_x509write_crt_free(&writecrt);
    mbedtls_entropy_free(&entropy);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    free(output_buf);
    return ret;
}

int ra_tls_create_key_and_crt(mbedtls_pk_context* key, mbedtls_x509_crt* crt) {
    return create_key_and_crt(key, crt, NULL, NULL);
}

int ra_tls_create_key_and_crt_der(uint8_t* der_key, size_t* der_key_size,
                                  uint8_t* der_crt, size_t* der_crt_size) {
    int ret;

    mbedtls_pk_context key;
    mbedtls_pk_init(&key);

    uint8_t* output_buf    = NULL;
    size_t output_buf_size = 1024;  /* enough for any public key */

    output_buf = malloc(output_buf_size);
    if (!output_buf) {
        ret = -1;
        goto out;
    }

    ret = create_key_and_crt(&key, NULL, der_crt, der_crt_size);
    if (ret < 0) {
        ret = -1;
        goto out;
    }

    /* populate der_key; note that der_crt was already populated */
    int size = mbedtls_pk_write_key_der(&key, output_buf, sizeof(output_buf));
    if (size < 0) {
        ret = -1;
        goto out;
    }

    /* note that previous function wrote data at the end of the output_buf */
    if (*der_key_size >= size) {
        /* populate der_key only if user provided sufficiently big buffer */
        memcpy(der_key, output_buf + sizeof(output_buf) - size, size);
    }
    *der_key_size = size;

    ret = 0;
out:
    mbedtls_pk_free(&key);
    free(output_buf);
    return ret;
}
