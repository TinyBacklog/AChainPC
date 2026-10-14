// SPDX-License-Identifier: GPL-2.0-or-later
// Emits an executable test of the SHA-1 crypto extension instructions.
//
// A transcricao segue a definicao portatil do dynarmic, por isso um teste
// escrito da mesma fonte nao provaria nada. O que prova e montar um SHA-1
// completo com as seis instrucoes e comparar com o OpenSSL.
#include <cassert>
#include <iostream>
#include <string>
#include "core/recompiler/arm64_to_c.h"

static std::string RuntimeSection() {
    const std::string rt = suyu::recomp::RuntimeC();
    const size_t a = rt.find("static uint32_t _sha_ror32(uint32_t x, unsigned n)");
    assert(a != std::string::npos && "SHA-1 helpers missing from RuntimeC()");
    const size_t b = rt.find("/* \xe2\x94\x80\xe2\x94\x80 Reciprocal estimates", a);
    assert(b != std::string::npos && "end marker missing");
    return rt.substr(a, b - a);
}

int main() {
    std::cout << R"C(
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <openssl/evp.h>
typedef struct GuestContext { uint64_t vreg[32][2]; } GuestContext;
)C";
    std::cout << RuntimeSection();

    unsigned slot = 0;
    std::string dispatch;
    auto emit = [&](uint32_t insn) {
        std::string body;
        if (!suyu::recomp::Translate(insn, 0, body) ||
            body.find("recomp_unhandled") != std::string::npos) {
            std::cerr << "not translated: " << std::hex << insn << '\n';
            std::exit(1);
        }
        std::cout << "static void f" << slot << "(GuestContext* c){" << body << "}\n";
        dispatch += "f" + std::to_string(slot) + ",";
        ++slot;
    };
    // 0..2: SHA1C / SHA1P / SHA1M  com Vd=0, Vn=1, Vm=2
    for (unsigned k = 0; k < 3; ++k)
        emit(0x5E000000u | (k << 12) | (2u << 16) | (1u << 5) | 0u);
    emit(0x5E003000u | (2u << 16) | (1u << 5) | 0u);   // 3: SHA1SU0 Vd=0,Vn=1,Vm=2
    emit(0x5E280800u | (1u << 5) | 0u);                // 4: SHA1H   Vd=0,Vn=1
    emit(0x5E281800u | (1u << 5) | 0u);                // 5: SHA1SU1 Vd=0,Vn=1

    std::cout << "static void (*functions[])(GuestContext*) = {" << dispatch << "};\n";
    std::cout << R"C(
static int checks = 0;

/* Chama uma instrucao copiando os operandos para os registos que ela usa. */
static void run3(int idx, uint32_t* d, const uint32_t* n, const uint32_t* m) {
    GuestContext c;
    memset(&c, 0, sizeof c);
    memcpy(c.vreg[0], d, 16);
    memcpy(c.vreg[1], n, 16);
    memcpy(c.vreg[2], m, 16);
    functions[idx](&c);
    memcpy(d, c.vreg[0], 16);
}
static void run2(int idx, uint32_t* d, const uint32_t* n) {
    GuestContext c;
    memset(&c, 0, sizeof c);
    memcpy(c.vreg[0], d, 16);
    memcpy(c.vreg[1], n, 16);
    functions[idx](&c);
    memcpy(d, c.vreg[0], 16);
}

/* SHA-1 de um bloco de 64 bytes, montado com as instrucoes. O agendamento de
   mensagem usa SHA1SU0 e SHA1SU1; as rondas usam SHA1C/P/M e SHA1H. */
static void sha1_block_via_insns(uint32_t* h, const uint8_t* block) {
    static const uint32_t K[4] = {0x5a827999u, 0x6ed9eba1u, 0x8f1bbcdcu, 0xca62c1d6u};
    uint32_t w[4][4];                 /* w[j] = W[4j .. 4j+3] */
    uint32_t abcd[4] = {h[0], h[1], h[2], h[3]};
    uint32_t e[4] = {h[4], 0, 0, 0};
    int g, i, j;
    for (j = 0; j < 4; ++j)
        for (i = 0; i < 4; ++i) {
            const int t = j * 4 + i;
            w[j][i] = ((uint32_t)block[t*4] << 24) | ((uint32_t)block[t*4+1] << 16) |
                      ((uint32_t)block[t*4+2] << 8) | (uint32_t)block[t*4+3];
        }
    for (g = 0; g < 20; ++g) {            /* 20 grupos de 4 rondas = 80 rondas */
        const int kidx = g / 5;
        uint32_t wk[4], tmp[4];
        for (i = 0; i < 4; ++i) wk[i] = w[g & 3][i] + K[kidx];
        /* o novo e e SHA1H do a anterior ao grupo */
        tmp[0] = abcd[0]; tmp[1] = tmp[2] = tmp[3] = 0;
        run2(4, tmp, tmp);                /* SHA1H: le Vn, escreve Vd */
        { uint32_t src[4]; src[0] = abcd[0]; src[1] = src[2] = src[3] = 0;
          tmp[0] = 0; tmp[1] = tmp[2] = tmp[3] = 0;
          run2(4, tmp, src); }
        { const int which = (kidx == 0) ? 0 : (kidx == 2) ? 2 : 1;  /* C, M, P */
          run3(which, abcd, e, wk); }
        memcpy(e, tmp, 16);
        if (g < 16) {                     /* estender o agendamento */
            const int a0 = g & 3, a1 = (g + 1) & 3, a2 = (g + 2) & 3, a3 = (g + 3) & 3;
            run3(3, w[a0], w[a1], w[a2]); /* SHA1SU0 */
            run2(5, w[a0], w[a3]);        /* SHA1SU1 */
        }
    }
    h[0] += abcd[0]; h[1] += abcd[1]; h[2] += abcd[2]; h[3] += abcd[3]; h[4] += e[0];
}

int main(void) {
    int t, i;
    /* SHA1H isolado: rotacao de 2 a direita no elemento baixo, resto a zero. */
    for (t = 0; t < 8; ++t) {
        uint32_t d[4] = {0xdeadbeefu, 1, 2, 3}, n[4];
        n[0] = 0x01234567u * (uint32_t)(t + 1); n[1] = n[2] = n[3] = 0xffffffffu;
        run2(4, d, n);
        { const uint32_t want = (n[0] >> 2) | (n[0] << 30);
          if (d[0] != want || d[1] || d[2] || d[3]) { printf("FAIL sha1h t=%d\n", t); return 1; } }
        ++checks;
    }

    /* A prova: SHA-1 completo de varias mensagens contra o OpenSSL. */
    for (t = 0; t < 6; ++t) {
        uint8_t msg[64];
        uint8_t padded[64];
        uint32_t h[5] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u, 0xc3d2e1f0u};
        uint8_t ours[20], ossl[20];
        unsigned len = 0;
        const size_t n = (size_t)t * 9;          /* 0, 9, 18, 27, 36, 45 bytes */
        for (i = 0; i < 64; ++i) msg[i] = (uint8_t)(t * 37 + i * 11 + 3);
        /* um unico bloco: mensagem curta com padding do FIPS */
        memset(padded, 0, sizeof padded);
        memcpy(padded, msg, n);
        padded[n] = 0x80;
        { const uint64_t bits = (uint64_t)n * 8;
          for (i = 0; i < 8; ++i) padded[56 + i] = (uint8_t)(bits >> (8 * (7 - i))); }
        sha1_block_via_insns(h, padded);
        for (i = 0; i < 5; ++i) {
            ours[i*4+0] = (uint8_t)(h[i] >> 24); ours[i*4+1] = (uint8_t)(h[i] >> 16);
            ours[i*4+2] = (uint8_t)(h[i] >> 8);  ours[i*4+3] = (uint8_t)h[i];
        }
        { EVP_MD_CTX* ctx = EVP_MD_CTX_new();
          EVP_DigestInit_ex(ctx, EVP_sha1(), NULL);
          EVP_DigestUpdate(ctx, msg, n);
          EVP_DigestFinal_ex(ctx, ossl, &len);
          EVP_MD_CTX_free(ctx); }
        if (memcmp(ours, ossl, 20)) {
            printf("FAIL sha1 t=%d (n=%zu)\n  nosso:", t, n);
            for (i = 0; i < 20; ++i) printf(" %02x", ours[i]);
            printf("\n  ossl :");
            for (i = 0; i < 20; ++i) printf(" %02x", ossl[i]);
            printf("\n");
            return 1;
        }
        ++checks;
    }
    if (sizeof functions / sizeof functions[0] != 6) return 1;
    printf("%d SHA-1 comparisons passed\n", checks);
    return 0;
}
)C";
    return 0;
}
