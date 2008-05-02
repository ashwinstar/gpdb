/* sign.c */

#include "chubby_private.h"

/*
 **********************************************************************
 ** md5.h -- Header file for implementation of MD5                   **
 ** RSA Data Security, Inc. MD5 Message Digest Algorithm             **
 ** Created: 2/17/90 RLR                                             **
 ** Revised: 12/27/90 SRD,AJ,BSK,JT Reference C version              **
 ** Revised (for MD5): RLR 4/27/91                                   **
 **   -- G modified to have y&~z instead of y&z                      **
 **   -- FF, GG, HH modified to add in last register done            **
 **   -- Access pattern: round 2 works mod 5, round 3 works mod 3    **
 **   -- distinct additive constant for each step                    **
 **   -- round 4 added, working mod 7                                **
 **********************************************************************
 */

/*
 **********************************************************************
 ** Copyright (C) 1990, RSA Data Security, Inc. All rights reserved. **
 **                                                                  **
 ** License to copy and use this software is granted provided that   **
 ** it is identified as the "RSA Data Security, Inc. MD5 Message     **
 ** Digest Algorithm" in all material mentioning or referencing this **
 ** software or this function.                                       **
 **                                                                  **
 ** License is also granted to make and use derivative works         **
 ** provided that such works are identified as "derived from the RSA **
 ** Data Security, Inc. MD5 Message Digest Algorithm" in all         **
 ** material mentioning or referencing the derived work.             **
 **                                                                  **
 ** RSA Data Security, Inc. makes no representations concerning      **
 ** either the merchantability of this software or the suitability   **
 ** of this software for any particular purpose.  It is provided "as **
 ** is" without express or implied warranty of any kind.             **
 **                                                                  **
 ** These notices must be retained in any copies of any part of this **
 ** documentation and/or software.                                   **
 **********************************************************************
 */

/* Data structure for MD5 (Message Digest) computation */
typedef struct {
  uint32_t i[2];                   /* number of _bits_ handled mod 2^64 */
  uint32_t buf[4];                                    /* scratch buffer */
  unsigned char in[64];                              /* input buffer */
  unsigned char digest[16];     /* actual digest after MD5Final call */
} chubby_MD5_CTX;

/*
 **********************************************************************
 ** End of md5.h                                                     **
 ******************************* (cut) ********************************
 */

/*
 **********************************************************************
 ** md5.c                                                            **
 ** RSA Data Security, Inc. MD5 Message Digest Algorithm             **
 ** Created: 2/17/90 RLR                                             **
 ** Revised: 1/91 SRD,AJ,BSK,JT Reference C Version                  **
 **********************************************************************
 */

/*
 **********************************************************************
 ** Copyright (C) 1990, RSA Data Security, Inc. All rights reserved. **
 **                                                                  **
 ** License to copy and use this software is granted provided that   **
 ** it is identified as the "RSA Data Security, Inc. MD5 Message     **
 ** Digest Algorithm" in all material mentioning or referencing this **
 ** software or this function.                                       **
 **                                                                  **
 ** License is also granted to make and use derivative works         **
 ** provided that such works are identified as "derived from the RSA **
 ** Data Security, Inc. MD5 Message Digest Algorithm" in all         **
 ** material mentioning or referencing the derived work.             **
 **                                                                  **
 ** RSA Data Security, Inc. makes no representations concerning      **
 ** either the merchantability of this software or the suitability   **
 ** of this software for any particular purpose.  It is provided "as **
 ** is" without express or implied warranty of any kind.             **
 **                                                                  **
 ** These notices must be retained in any copies of any part of this **
 ** documentation and/or software.                                   **
 **********************************************************************
 */

/* -- include the following line if the md5.h header file is separate -- */
/* #include "md5.h" */

/* forward declaration */
static void Transform ();

static unsigned char PADDING[64] = {
  0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

/* F, G and H are basic MD5 functions: selection, majority, parity */
#define F(x, y, z) (((x) & (y)) | ((~x) & (z)))
#define G(x, y, z) (((x) & (z)) | ((y) & (~z)))
#define H(x, y, z) ((x) ^ (y) ^ (z))
#define I(x, y, z) ((y) ^ ((x) | (~z))) 

/* ROTATE_LEFT rotates x left n bits */
#define ROTATE_LEFT(x, n) (((x) << (n)) | ((x) >> (32-(n))))

/* FF, GG, HH, and II transformations for rounds 1, 2, 3, and 4 */
/* Rotation is separate from addition to prevent recomputation */
#define FF(a, b, c, d, x, s, ac) \
  {(a) += F ((b), (c), (d)) + (x) + (uint32_t)(ac); \
   (a) = ROTATE_LEFT ((a), (s)); \
   (a) += (b); \
  }
#define GG(a, b, c, d, x, s, ac) \
  {(a) += G ((b), (c), (d)) + (x) + (uint32_t)(ac); \
   (a) = ROTATE_LEFT ((a), (s)); \
   (a) += (b); \
  }
#define HH(a, b, c, d, x, s, ac) \
  {(a) += H ((b), (c), (d)) + (x) + (uint32_t)(ac); \
   (a) = ROTATE_LEFT ((a), (s)); \
   (a) += (b); \
  }
#define II(a, b, c, d, x, s, ac) \
  {(a) += I ((b), (c), (d)) + (x) + (uint32_t)(ac); \
   (a) = ROTATE_LEFT ((a), (s)); \
   (a) += (b); \
  }

static void
chubby_MD5Init (mdContext)
chubby_MD5_CTX *mdContext;
{
  mdContext->i[0] = mdContext->i[1] = (uint32_t)0;

  /* Load magic initialization constants.
   */
  mdContext->buf[0] = (uint32_t)0x67452301;
  mdContext->buf[1] = (uint32_t)0xefcdab89;
  mdContext->buf[2] = (uint32_t)0x98badcfe;
  mdContext->buf[3] = (uint32_t)0x10325476;
}

static void
chubby_MD5Update (mdContext, inBuf, inLen)
chubby_MD5_CTX *mdContext;
unsigned char *inBuf;
unsigned int inLen;
{
  uint32_t in[16];
  int mdi;
  unsigned int i, ii;

  /* compute number of bytes mod 64 */
  mdi = (int)((mdContext->i[0] >> 3) & 0x3F);

  /* update number of bits */
  if ((mdContext->i[0] + ((uint32_t)inLen << 3)) < mdContext->i[0])
    mdContext->i[1]++;
  mdContext->i[0] += ((uint32_t)inLen << 3);
  mdContext->i[1] += ((uint32_t)inLen >> 29);

  while (inLen--) {
    /* add new character to buffer, increment mdi */
    mdContext->in[mdi++] = *inBuf++;

    /* transform if necessary */
    if (mdi == 0x40) {
      for (i = 0, ii = 0; i < 16; i++, ii += 4)
        in[i] = (((uint32_t)mdContext->in[ii+3]) << 24) |
                (((uint32_t)mdContext->in[ii+2]) << 16) |
                (((uint32_t)mdContext->in[ii+1]) << 8) |
                ((uint32_t)mdContext->in[ii]);
      Transform (mdContext->buf, in);
      mdi = 0;
    }
  }
}

static void
chubby_MD5Final (mdContext)
chubby_MD5_CTX *mdContext;
{
  uint32_t in[16];
  int mdi;
  unsigned int i, ii;
  unsigned int padLen;

  /* save number of bits */
  in[14] = mdContext->i[0];
  in[15] = mdContext->i[1];

  /* compute number of bytes mod 64 */
  mdi = (int)((mdContext->i[0] >> 3) & 0x3F);

  /* pad out to 56 mod 64 */
  padLen = (mdi < 56) ? (56 - mdi) : (120 - mdi);
  chubby_MD5Update (mdContext, PADDING, padLen);

  /* append length in bits and transform */
  for (i = 0, ii = 0; i < 14; i++, ii += 4)
    in[i] = (((uint32_t)mdContext->in[ii+3]) << 24) |
            (((uint32_t)mdContext->in[ii+2]) << 16) |
            (((uint32_t)mdContext->in[ii+1]) << 8) |
            ((uint32_t)mdContext->in[ii]);
  Transform (mdContext->buf, in);

  /* store buffer in digest */
  for (i = 0, ii = 0; i < 4; i++, ii += 4) {
    mdContext->digest[ii] = (unsigned char)(mdContext->buf[i] & 0xFF);
    mdContext->digest[ii+1] =
      (unsigned char)((mdContext->buf[i] >> 8) & 0xFF);
    mdContext->digest[ii+2] =
      (unsigned char)((mdContext->buf[i] >> 16) & 0xFF);
    mdContext->digest[ii+3] =
      (unsigned char)((mdContext->buf[i] >> 24) & 0xFF);
  }
}

/* Basic MD5 step. Transform buf based on in.
 */
static void Transform (buf, in)
uint32_t *buf;
uint32_t *in;
{
  uint32_t a = buf[0], b = buf[1], c = buf[2], d = buf[3];

  /* Round 1 */
#define S11 7
#define S12 12
#define S13 17
#define S14 22

  FF ( a, b, c, d, in[ 0], S11, -680876936); /* 1 */
  FF ( d, a, b, c, in[ 1], S12, -389564586); /* 2 */
  FF ( c, d, a, b, in[ 2], S13,  606105819); /* 3 */
  FF ( b, c, d, a, in[ 3], S14,-1044525330); /* 4 */
  FF ( a, b, c, d, in[ 4], S11, -176418897); /* 5 */
  FF ( d, a, b, c, in[ 5], S12, 1200080426); /* 6 */
  FF ( c, d, a, b, in[ 6], S13,-1473231341); /* 7 */
  FF ( b, c, d, a, in[ 7], S14,  -45705983); /* 8 */
  FF ( a, b, c, d, in[ 8], S11, 1770035416); /* 9 */
  FF ( d, a, b, c, in[ 9], S12,-1958414417); /* 10 */
  FF ( c, d, a, b, in[10], S13,     -42063); /* 11 */
  FF ( b, c, d, a, in[11], S14,-1990404162); /* 12 */
  FF ( a, b, c, d, in[12], S11, 1804603682); /* 13 */
  FF ( d, a, b, c, in[13], S12,  -40341101); /* 14 */
  FF ( c, d, a, b, in[14], S13,-1502002290); /* 15 */
  FF ( b, c, d, a, in[15], S14, 1236535329); /* 16 */

  /* Round 2 */
#define S21 5
#define S22 9
#define S23 14
#define S24 20
  GG ( a, b, c, d, in[ 1], S21, -165796510); /* 17 */
  GG ( d, a, b, c, in[ 6], S22,-1069501632); /* 18 */
  GG ( c, d, a, b, in[11], S23,  643717713); /* 19 */
  GG ( b, c, d, a, in[ 0], S24, -373897302); /* 20 */
  GG ( a, b, c, d, in[ 5], S21, -701558691); /* 21 */
  GG ( d, a, b, c, in[10], S22,   38016083); /* 22 */
  GG ( c, d, a, b, in[15], S23, -660478335); /* 23 */
  GG ( b, c, d, a, in[ 4], S24, -405537848); /* 24 */
  GG ( a, b, c, d, in[ 9], S21,  568446438); /* 25 */
  GG ( d, a, b, c, in[14], S22,-1019803690); /* 26 */
  GG ( c, d, a, b, in[ 3], S23, -187363961); /* 27 */
  GG ( b, c, d, a, in[ 8], S24, 1163531501); /* 28 */
  GG ( a, b, c, d, in[13], S21,-1444681467); /* 29 */
  GG ( d, a, b, c, in[ 2], S22,  -51403784); /* 30 */
  GG ( c, d, a, b, in[ 7], S23, 1735328473); /* 31 */
  GG ( b, c, d, a, in[12], S24,-1926607734); /* 32 */

  /* Round 3 */
#define S31 4
#define S32 11
#define S33 16
#define S34 23
  HH ( a, b, c, d, in[ 5], S31,    -378558); /* 33 */
  HH ( d, a, b, c, in[ 8], S32,-2022574463); /* 34 */
  HH ( c, d, a, b, in[11], S33, 1839030562); /* 35 */
  HH ( b, c, d, a, in[14], S34,  -35309556); /* 36 */
  HH ( a, b, c, d, in[ 1], S31,-1530992060); /* 37 */
  HH ( d, a, b, c, in[ 4], S32, 1272893353); /* 38 */
  HH ( c, d, a, b, in[ 7], S33, -155497632); /* 39 */
  HH ( b, c, d, a, in[10], S34,-1094730640); /* 40 */
  HH ( a, b, c, d, in[13], S31,  681279174); /* 41 */
  HH ( d, a, b, c, in[ 0], S32, -358537222); /* 42 */
  HH ( c, d, a, b, in[ 3], S33, -722521979); /* 43 */
  HH ( b, c, d, a, in[ 6], S34,   76029189); /* 44 */
  HH ( a, b, c, d, in[ 9], S31, -640364487); /* 45 */
  HH ( d, a, b, c, in[12], S32, -421815835); /* 46 */
  HH ( c, d, a, b, in[15], S33,  530742520); /* 47 */
  HH ( b, c, d, a, in[ 2], S34, -995338651); /* 48 */

  /* Round 4 */
#define S41 6
#define S42 10
#define S43 15
#define S44 21
  II ( a, b, c, d, in[ 0], S41, -198630844); /* 49 */
  II ( d, a, b, c, in[ 7], S42, 1126891415); /* 50 */
  II ( c, d, a, b, in[14], S43,-1416354905); /* 51 */
  II ( b, c, d, a, in[ 5], S44,  -57434055); /* 52 */
  II ( a, b, c, d, in[12], S41, 1700485571); /* 53 */
  II ( d, a, b, c, in[ 3], S42,-1894986606); /* 54 */
  II ( c, d, a, b, in[10], S43,   -1051523); /* 55 */
  II ( b, c, d, a, in[ 1], S44,-2054922799); /* 56 */
  II ( a, b, c, d, in[ 8], S41, 1873313359); /* 57 */
  II ( d, a, b, c, in[15], S42,  -30611744); /* 58 */
  II ( c, d, a, b, in[ 6], S43,-1560198380); /* 59 */
  II ( b, c, d, a, in[13], S44, 1309151649); /* 60 */
  II ( a, b, c, d, in[ 4], S41, -145523070); /* 61 */
  II ( d, a, b, c, in[11], S42,-1120210379); /* 62 */
  II ( c, d, a, b, in[ 2], S43,  718787259); /* 63 */
  II ( b, c, d, a, in[ 9], S44, -343485551); /* 64 */

  buf[0] += a;
  buf[1] += b;
  buf[2] += c;
  buf[3] += d;
}

/*
 **********************************************************************
 ** End of md5.c                                                     **
 ******************************* (cut) ********************************
 */

int
chubby_readPaxosKey(chubby_t*thiz)
{
    char p[PATH_MAX],*q;
    FILE*f;
    int i;
    long filesize;
    sprintf(p,"%s/.paxos_key",getenv("HOME"));
    f = fopen(p,"r");
    if (!f){
        perror(p);
	return 1;
    }
    fseek(f,0,2);
    filesize = ftell(f);
    rewind(f);
    q = thiz->malloc(filesize+1);
    if (!q)
        return 1;
    thiz->paxosKey = q;
    while ((i=getc(f))!=-1)
        if (i>' ' && i<='~' && q<thiz->paxosKey+filesize)
	    *q++ = i;
    *q = 0;
    return 0;
}

void
sockaddr_toString(char*a,const struct sockaddr*sa)
{
    assert(sa->sa_family==AF_INET);
    sprintf(a,"%u.%u.%u.%u:%u ",sa->sa_data[2]&255,sa->sa_data[3]&255,sa->sa_data[4]&255,sa->sa_data[5]&255,(sa->sa_data[0]&255)<<8|(sa->sa_data[1]&255));
}

void
chubby_sign(chubby_t*thiz,char*a,const char*b,int len,const struct sockaddr*f,const struct sockaddr*t)
{
    char s[23]; /* "xxx.xxx.xxx.xxx:xxxxx " */
    int i;
    chubby_MD5_CTX m;
    chubby_MD5Init(&m);
    chubby_MD5Update(&m,thiz->paxosKey,strlen(thiz->paxosKey));
    sockaddr_toString(s,f);
    chubby_MD5Update(&m,s,strlen(s));
    sockaddr_toString(s,t);
    chubby_MD5Update(&m,s,strlen(s));
    chubby_MD5Update(&m,b+SIGNSTRING_LENGTH,len-SIGNSTRING_LENGTH);
    chubby_MD5Final(&m);
    for (i=0; i<16; i++)
        sprintf(a+i*2,"%02x",m.digest[i]);
    a[32] = ' ';
}
