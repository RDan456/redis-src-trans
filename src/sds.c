#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>
#include <limits.h>
#include "sds.h"
#include "sdsalloc.h"

const char *SDS_NOINIT = "SDS_NOINIT";

///静态内联函数，用来获取各种sds结构体的长度，分别是sdshdr5、sdshdr8、sdshdr16、sdshdr32、sdshdr64
static inline int sdsHdrSize(char type) {
    switch(type&SDS_TYPE_MASK) {
        case SDS_TYPE_5:
            return sizeof(struct sdshdr5);
        case SDS_TYPE_8:
            return sizeof(struct sdshdr8);
        case SDS_TYPE_16:
            return sizeof(struct sdshdr16);
        case SDS_TYPE_32:
            return sizeof(struct sdshdr32);
        case SDS_TYPE_64:
            return sizeof(struct sdshdr64);
    }
    return 0;
}

///静态内联函数，用来获取sds类型，
static inline char sdsReqType(size_t string_size) {
    if (string_size < 1<<5) ///如果长度小于2^6，则为SDS_TYPE_5类型
        return SDS_TYPE_5;
    if (string_size < 1<<8) ///如果长度小于2^8，则为SDS_TYPE_5类型
        return SDS_TYPE_8;
    if (string_size < 1<<16) ///如果长度小于2^16，则为SDS_TYPE_16类型
        return SDS_TYPE_16;
#if (LONG_MAX == LLONG_MAX)
    if (string_size < 1<<32) ///如果长度小于2^32，则为SDS_TYPE_5类型
        return SDS_TYPE_32;
    return SDS_TYPE_64;
#else
    return SDS_TYPE_32;
#endif
}

///用'init'指针和'initlen'指定的内容创建一个新的sds字符串。如果在“init”中使用NULL，则字符串将以零字节初始化。如果使用SDS_NOINIT，则缓冲区未初始化;
///字符串总是以null结尾(所有的sds字符串都是，总是)所以即使你创建一个sds字符串:
///mystring = sdsnewlen (“abc”, 3);
///可以使用printf()打印字符串，因为字符串末尾有一个隐式\0。但是，字符串是二进制安全的，中间可以包含\0字符，因为长度存储在sds头中。
sds sdsnewlen(const void *init, size_t initlen) {
    void *sh;
    sds s;
    char type = sdsReqType(initlen); ///获取sds结构体的类型，为SDS_TYPE_5、SDS_TYPE_8、SDS_TYPE_16、SDS_TYPE_32、SDS_TYPE_64中的一种
    
    ///如果它的类型为SDS_TYPE_5类型，则将其转化为SDS_TYPE_8类型
    if (type == SDS_TYPE_5 && initlen == 0) type = SDS_TYPE_8;
    int hdrlen = sdsHdrSize(type); ///通过类型，获取到sds的头的长度
    unsigned char *fp; ///用来表示flags的指针

    sh = s_malloc(hdrlen+initlen+1); ///为sds申请内存空间，空间的大小为hdrlen（sds头） + initlen + 1（'\0'）。
    if (sh == NULL) return NULL; ///申请失败，就直接返回
    if (init==SDS_NOINIT) ///如果init为no_init,需要将申请的初始化为0
        init = NULL;
    else if (!init)
        memset(sh, 0, hdrlen+initlen+1); ///将sh置为0
    s = (char*)sh+hdrlen; ///返回s，它的内存地址为sh向右偏移hdrlen（sds头的长度）长度。
    fp = ((unsigned char*)s)-1; ///获取flags的类型。
    switch(type) {
        case SDS_TYPE_5: {
            *fp = type | (initlen << SDS_TYPE_BITS);
            break;
        }
        case SDS_TYPE_8: { ///如果是SDS_TYPE_8类型
            SDS_HDR_VAR(8,s); ///获取sds的头部开始地址
            sh->len = initlen; ///从头部中获取字符串的长度
            sh->alloc = initlen;///从头部头获取buf[]空间的长度
            *fp = type; ///设置flag的值，为type，也就是1
            break;
        }
        case SDS_TYPE_16: {
            SDS_HDR_VAR(16,s);
            sh->len = initlen;
            sh->alloc = initlen;
            *fp = type;///设置flag的值，为type，也就是2
            break;
        }
        case SDS_TYPE_32: {
            SDS_HDR_VAR(32,s);
            sh->len = initlen;
            sh->alloc = initlen;
            *fp = type;///设置flag的值，为type，也就是3
            break;
        }
        case SDS_TYPE_64: {
            SDS_HDR_VAR(64,s);
            sh->len = initlen;
            sh->alloc = initlen;
            *fp = type;///设置flag的值，为type，也就是4
            break;
        }
    }
    if (initlen && init)
        memcpy(s, init, initlen); ///将init中的值拷贝到s中
    s[initlen] = '\0'; ///将最后一个位置设置为c语言字符串结尾的标志'\0'
    return s; ///返回新创建的字符串
}

///创建一个新的字符串，这个字符串值包含包头和'\0'，字符串的内容为空
sds sdsempty(void) {
    return sdsnewlen("",0);
}

///创建一个新的sds，传入一个char类型的指针，通过sdsnewlen()函数来创建
sds sdsnew(const char *init) {
    size_t initlen = (init == NULL) ? 0 : strlen(init);
    return sdsnewlen(init, initlen);
}

///拷贝一个字符串
sds sdsdup(const sds s) {
    return sdsnewlen(s, sdslen(s));
}

///释放一个字符串的空间
void sdsfree(sds s) {
    if (s == NULL) return;
    ///解释一下: s[-1]表示字符串的头部的地址；sdsHdrSize为获取头部的大小
    s_free((char*)s-sdsHdrSize(s[-1]));
}

/**
 * 将sds字符串长度设置为使用strlen()获得的长度，因此只考虑第一个空字符之前的内容。当以某种方式手动破解sds字符串时，这个函数非常有用，如下例所示:
 *
 * s = sdsnew (“foobar”);
 * s [2] = ' \ 0 ';
 * sdsupdatelen(s);
 * printf (" % d \ n”, sdslen (s));
 *
 * 输出将是“2”，但是如果我们注释掉对sdsupdatelen()的调用，输出将是“6”，因为字符串被修改了，但是逻辑长度仍然是6字节。
 */
///更新字符串的长度，具体集合上面的例子
void sdsupdatelen(sds s) {
    size_t reallen = strlen(s);
    sdssetlen(s, reallen);
}

///将字符串重置保存空间，惰性释放
void sdsclear(sds s) {
    sdssetlen(s, 0); //
    s[0] = '\0';
}

///扩大sds字符串末尾的空闲空间，以便调用者确保在调用此函数后可以覆盖字符串末尾的addlen字节，以及nul项的多一个字节。
///注意:这不会改变sdslen()返回的sds字符串的*长度*，只会改变我们拥有的空闲缓冲区空间。
///对sds中的buf[]数组进行扩张
sds sdsMakeRoomFor(sds s, size_t addlen) {
    void *sh, *newsh;
    size_t avail = sdsavail(s); ///获取sds中buf[]的可用空间
    size_t len, newlen;
    char type, oldtype = s[-1] & SDS_TYPE_MASK; ///
    int hdrlen;

    if (avail >= addlen) return s; ///如果buf[]中的可用空间大于我们需要的空间，就直接返回

    len = sdslen(s); ///获取sds中字符串的长度，也就是buf[]中已用空间的长度
    sh = (char*)s-sdsHdrSize(oldtype); ///sh指向字符串开始的位置
    newlen = (len+addlen); ///进行扩展后新的空间的长度，大小为len + addlen
    if (newlen < SDS_MAX_PREALLOC) ///如果说新的长度小于“字符串预分配长度（1024 * 1024）”,就将字符串长度申请为newlen的两倍，主要是为了避免频繁的操作内存空间
        newlen *= 2;
    else
        newlen += SDS_MAX_PREALLOC; ///否则新的长度为newlen + SDS_MXS_PREALLOC

    type = sdsReqType(newlen); ///分配了新的长度，它的type可能会发生改变，所以在这里需要重新获取它的类型

    ///如果是SDS_TYPE_5类型，就将其设置为SDS_TYPE_8
    if (type == SDS_TYPE_5) type = SDS_TYPE_8;

    hdrlen = sdsHdrSize(type); ///获取头部的长度，这样才能够确认字符串开始的位置
    if (oldtype==type) { ///如果说扩展前后type没有发生改变
        newsh = s_realloc(sh, hdrlen+newlen+1); ///申请新的地址空间k并获取d它的地址
        if (newsh == NULL) return NULL;
        s = (char*)newsh+hdrlen; ///获取字符串开始的位置
    } else {
        ///由于head大小变化，需要向前移动字符串，不能使用realloc
        newsh = s_malloc(hdrlen+newlen+1); ///申请地址空间
        if (newsh == NULL) return NULL;
        memcpy((char*)newsh+hdrlen, s, len+1); ///将字符串中的内容复制到新的地址空间中
        s_free(sh); ///释放原来sds的地址空间
        s = (char*)newsh+hdrlen; ///获取新字符串开始的位置
        s[-1] = type; ///设置f字符串内存
        sdssetlen(s, len); ///设置len
    }
    sdssetalloc(s, newlen);///设置alloc
    return s;
}

/// 重新分配sds字符串，使其末尾没有空闲空间。所包含的字符串保持不变，但接下来的连接操作将需要重新分配。
/// 在调用之后，传递的sds字符串不再有效，所有引用必须用调用返回的新指针替换。
sds sdsRemoveFreeSpace(sds s) {
    void *sh, *newsh;
    char type, oldtype = s[-1] & SDS_TYPE_MASK;
    int hdrlen, oldhdrlen = sdsHdrSize(oldtype);
    size_t len = sdslen(s);
    size_t avail = sdsavail(s);
    sh = (char*)s-oldhdrlen;

    if (avail == 0) return s;///如果说sds中的buf[]可用空间已经为0了，就直接将其返回

    type = sdsReqType(len); ///检查刚好适合此字符串的最小SDS的type
    hdrlen = sdsHdrSize(type); ///检查刚好适合此字符串的最小SDS的head

    ///如果类型相同，或者至少仍然需要足够大的类型，则只需realloc()，让分配器仅在真正需要时进行复制。否则，如果更改很大，我们就手动重新分配字符串以使用不同的头文件类型。
    if (oldtype==type || type > SDS_TYPE_8) {
        newsh = s_realloc(sh, oldhdrlen+len+1); ///重新申请新的内存空间，大小和原来一样
        if (newsh == NULL) return NULL;
        s = (char*)newsh+oldhdrlen;
    } else {
        newsh = s_malloc(hdrlen+len+1); ///申请新的内存空间，大小为新的head大小+len+1
        if (newsh == NULL) return NULL;
        memcpy((char*)newsh+hdrlen, s, len+1); ///将原来的字符串拷贝到新的地址空间中
        s_free(sh); ///释放原来的字符串空间
        s = (char*)newsh+hdrlen; ///获取新字符串开始的位置
        s[-1] = type;
        sdssetlen(s, len);
    }
    sdssetalloc(s, len);
    return s;
}

/*返回指定sds字符串分配的总大小，
 *包括:
 * 1)指针前的sds头。
 * 2)字符串。
 * 3)末尾的空闲缓冲区(如果有的话)。
 * 4)隐含的无效术语（'\0'）。
 */
size_t sdsAllocSize(sds s) { ///获取sds分配的空间大小
    size_t alloc = sdsalloc(s);
    return sdsHdrSize(s[-1])+alloc+1;
}


void *sdsAllocPtr(sds s) { ///获取字符串开始位置
    return (void*) (s-sdsHdrSize(s[-1]));
}

/*根据“incr”递增sds长度并递减字符串末尾的剩余空间。还要在字符串的新末尾设置空词。
 *此函数用于在用户调用sdsMakeRoomFor()、在当前字符串末尾写入内容以及最后需要设置新长度后修正字符串长度。
 *注意:可以使用负增量来对字符串进行右减少。
 */
void sdsIncrLen(sds s, ssize_t incr) {
    unsigned char flags = s[-1]; ///获取sds的flag
    size_t len;
    switch(flags&SDS_TYPE_MASK) { ///通过与操作，获取sds类型
        case SDS_TYPE_5: { ///sds类型没有使用过，所以这里不再详解
            unsigned char *fp = ((unsigned char*)s)-1;
            unsigned char oldlen = SDS_TYPE_5_LEN(flags);
            assert((incr > 0 && oldlen+incr < 32) || (incr < 0 && oldlen >= (unsigned int)(-incr)));
            *fp = SDS_TYPE_5 | ((oldlen+incr) << SDS_TYPE_BITS);
            len = oldlen+incr;
            break;
        }
        case SDS_TYPE_8: { ///如果是SDS_TYPE_8类型
            SDS_HDR_VAR(8,s); ///获取sds的头部开始位置
            ///保证buf[]中可用的空间大小要大于需要的空间，否则直接跳出switch，退出程序
            assert((incr >= 0 && sh->alloc-sh->len >= incr) || (incr < 0 && sh->len >= (unsigned int)(-incr)));
            len = (sh->len += incr); ///sds新的len等于原来的len + incr
            break;
        }
        case SDS_TYPE_16: {
            SDS_HDR_VAR(16,s);
            assert((incr >= 0 && sh->alloc-sh->len >= incr) || (incr < 0 && sh->len >= (unsigned int)(-incr)));
            len = (sh->len += incr);
            break;
        }
        case SDS_TYPE_32: {
            SDS_HDR_VAR(32,s);
            assert((incr >= 0 && sh->alloc-sh->len >= (unsigned int)incr) || (incr < 0 && sh->len >= (unsigned int)(-incr)));
            len = (sh->len += incr);
            break;
        }
        case SDS_TYPE_64: {
            SDS_HDR_VAR(64,s);
            assert((incr >= 0 && sh->alloc-sh->len >= (uint64_t)incr) || (incr < 0 && sh->len >= (uint64_t)(-incr)));
            len = (sh->len += incr);
            break;
        }
        default: len = 0; ///没啥用，仅仅是为了避免警告
    }
    s[len] = '\0'; ///将字符串结尾的位置添上结尾标志'\0';
}

///使sds具有指定的长度。对新增的长度的位置初始化为0。如果指定的长度小于当前长度，则不执行任何操作。
sds sdsgrowzero(sds s, size_t len) {
    size_t curlen = sdslen(s);

    if (len <= curlen) return s; ///如果指定长度小于当前d长度，就直接返回
    s = sdsMakeRoomFor(s,len-curlen); ///扩张字符串
    if (s == NULL) return NULL;

    memset(s+curlen,0,(len-curlen+1)); ///将扩展位置处全部填充为0
    sdssetlen(s, len); ///设置新的len
    return s;
}

///将字符串t追加到字符串s的尾部
sds sdscatlen(sds s, const void *t, size_t len) {
    size_t curlen = sdslen(s); ///获取原来字符串的s的len

    s = sdsMakeRoomFor(s,len); ///对原来字符串进行hj扩展
    if (s == NULL) return NULL;
    memcpy(s+curlen, t, len); ///将t拷贝到s的后面
    sdssetlen(s, curlen+len); ///设置新字符串的len
    s[curlen+len] = '\0'; ///在新的字符串尾部加上结尾标志符
    return s;
}

///将t追加到s的尾部
sds sdscat(sds s, const char *t) {
    return sdscatlen(s, t, strlen(t));
}

///将t追加到s的尾部
sds sdscatsds(sds s, const sds t) {
    return sdscatlen(s, t, sdslen(t));
}

///将t拷贝到s中，就是用t的内容将s覆盖
sds sdscpylen(sds s, const char *t, size_t len) {
    if (sdsalloc(s) < len) { ///如果说s的大小小于t的大小，就需要进行扩容
        s = sdsMakeRoomFor(s,len-sdslen(s)); ///扩容，z扩展的大小为len-s的长度
        if (s == NULL) return NULL;
    }
    memcpy(s, t, len); ///将t的内容拷贝大s中
    s[len] = '\0'; ///设置结尾标志符
    sdssetlen(s, len); ///设置新字符串的len
    return s;
}

///将t拷贝到s中
sds sdscpy(sds s, const char *t) {
    return sdscpylen(s, t, strlen(t));
}

/*
 *用于sdscatlonglong()执行实际数字->字符串转换的助手。's'必须指向至少有SDS_LLSTR_SIZE字节空间的字符串。
 *
 *该函数返回存储在's'处的以空结尾的字符串的长度。
 */
#define SDS_LLSTR_SIZE 21
int sdsll2str(char *s, long long value) {
    char *p, aux;
    unsigned long long v;
    size_t l;

    v = (value < 0) ? -value : value;///获取v的绝对值
    p = s;
    do {
        *p++ = '0'+(v%10); ///利用取余法获取v中的每一个数字字符
        v /= 10; ///除以10，将v缩小
    } while(v);
    if (value < 0) *p++ = '-'; ///如果value小于0，需要将其加上一个‘-’号

    l = p-s;
    *p = '\0';

    ///上面的字符串p是反的，所以我们需要对p进行取放操作
    p--;
    while(s < p) {
        aux = *s;
        *s = *p;
        *p = aux;
        s++;
        p--;
    }
    return l;
}

/*相同的sdsll2str()，但用于无符号long long类型。*/
int sdsull2str(char *s, unsigned long long v) {
    char *p, aux;
    size_t l;

    p = s;
    do {
        *p++ = '0'+(v%10);
        v /= 10;
    } while(v);

    l = p-s;
    *p = '\0';

    p--;
    while(s < p) {
        aux = *s;
        *s = *p;
        *p = aux;
        s++;
        p--;
    }
    return l;
}

/*从一个很长的值创建一个sds字符串。它比:
 *
 * sdscatprintf (sdsempty (),“% lld \ n”,值);
 */
sds sdsfromlonglong(long long value) {
    char buf[SDS_LLSTR_SIZE];
    int len = sdsll2str(buf,value);

    return sdsnewlen(buf,len);///返回新的字符串
}

///类似于sdscatprintf()，但得到va_list而不是变量。
sds sdscatvprintf(sds s, const char *fmt, va_list ap) {
    va_list cpy;
    char staticbuf[1024], *buf = staticbuf, *t;
    size_t buflen = strlen(fmt)*2;

    /* We try to start using a static buffer for speed.
     * If not possible we revert to heap allocation. */
    if (buflen > sizeof(staticbuf)) { ///如果buflen的大小比staticbuf（1024）的长度要长，就申请buflen长度的内存空间
        buf = s_malloc(buflen);
        if (buf == NULL) return NULL;
    } else {
        buflen = sizeof(staticbuf); ///如果buflen的大小比staticbuf（1024）长度要小，就申请staticbuf长度的内存空间
    }

    /* Try with buffers two times bigger every time we fail to
     * fit the string in the current buffer size. */
    /*尝试使用两倍大的缓冲区，每次我们不能适应字符串在当前缓冲区的大小。*/
    while(1) {
        buf[buflen-2] = '\0';
        va_copy(cpy,ap); ///将ap中的内容copy到cpy中
        vsnprintf(buf, buflen, fmt, cpy);
        va_end(cpy);
        if (buf[buflen-2] != '\0') {
            if (buf != staticbuf) s_free(buf);
            buflen *= 2;
            buf = s_malloc(buflen);
            if (buf == NULL) return NULL;
            continue;
        }
        break;
    }

    /*最后将获得的字符串与SDS字符串连接并返回它。*/
    t = sdscat(s, buf);
    if (buf != staticbuf) s_free(buf);
    return t;
}

/* Append to the sds string 's' a string obtained using printf-alike format
 * specifier.
 *
 * After the call, the modified sds string is no longer valid and all the
 * references must be substituted with the new pointer returned by the call.
 *
 * Example:
 *
 * s = sdsnew("Sum is: ");
 * s = sdscatprintf(s,"%d+%d = %d",a,b,a+b).
 *
 * Often you need to create a string from scratch with the printf-alike
 * format. When this is the need, just use sdsempty() as the target string:
 *
 * s = sdscatprintf(sdsempty(), "... your format ...", args);
 */
///打印不定数量的字符串，采用的方式是将这个字符串追加到s的尾部
sds sdscatprintf(sds s, const char *fmt, ...) {
    va_list ap;
    char *t;
    va_start(ap, fmt);
    t = sdscatvprintf(s,fmt,ap);
    va_end(ap);
    return t;
}

/*这个函数类似于sdscatprintf，但是要快得多，因为它不依赖libc实现的sprintf()家族函数，后者通常非常慢。此外，在连接新数据时直接处理sds字符串可以提高性能。
 但是，这个函数只处理不兼容的类似printf的子集
 *格式说明符:
 *
 * %s - C字符串
 * %S - SDS字符串
 * %i - 符号int
 * %I - 64位带符号整数(long long, int64_t)
 * %u - 无符号int
 * %U - 64位无符号整数(unsigned long long, uint64_t)
 * %% - 逐字输入“%”字符。
 */
sds sdscatfmt(sds s, char const *fmt, ...) {
    size_t initlen = sdslen(s);
    const char *f = fmt;
    long i;
    va_list ap;

    /*为了避免连续的重新分配，让我们创建可以容纳至少两倍于格式字符串本身的缓冲区开始。这不是最好的启发式，但似乎在实践中有效。*/
    s = sdsMakeRoomFor(s, initlen + strlen(fmt)*2);
    va_start(ap,fmt);
    f = fmt;    ///下一个要处理的字符串
    i = initlen; ///要写入到dest str的下一个字节的位置。
    while(*f) {
        char next, *str;
        size_t l;
        long long num;
        unsigned long long unum;

        ///要保证sds中buf[]要有一个可用的字符空间
        if (sdsavail(s)==0) {
            s = sdsMakeRoomFor(s,1);
        }

        switch(*f) { ///用switch来判断*f字符的类型
        case '%': ///如果是%类型，将逐个的读入字符
            next = *(f+1); ///读取下一个字符
            f++;
            switch(next) {
            case 's': ///如果是c语言风格的char类型
            case 'S': ///如果是sdse类型
                str = va_arg(ap,char*);
                l = (next == 's') ? strlen(str) : sdslen(str);
                if (sdsavail(s) < l) { ///保证buf[]中至少有一个可用空间
                    s = sdsMakeRoomFor(s,l); ///申请内存空间
                }
                memcpy(s+i,str,l); ///将str拷贝到s+i处
                sdsinclen(s,l); ///调整s的len
                i += l; ///i向后移动l
                break;
            case 'i': ///如果是int类型
            case 'I': ///如果是64位带符号的整数
                if (next == 'i')
                    num = va_arg(ap,int);
                else
                    num = va_arg(ap,long long);
                {
                    char buf[SDS_LLSTR_SIZE];
                    l = sdsll2str(buf,num); ///将long long类型的数据转化为字符串存储
                    if (sdsavail(s) < l) {
                        s = sdsMakeRoomFor(s,l); ///空间如果不够，需要申请新的内存空间
                    }
                    memcpy(s+i,buf,l); ///将得到的字符串拷贝到s+i处
                    sdsinclen(s,l);///更新s的len
                    i += l; ///移动i到i+l
                }
                break;
            case 'u': ///如果是无符号整数
            case 'U': ///如果是无符号的long long类型的数据
                if (next == 'u')
                    unum = va_arg(ap,unsigned int);
                else
                    unum = va_arg(ap,unsigned long long);
                {
                    char buf[SDS_LLSTR_SIZE];
                    l = sdsull2str(buf,unum); ///将这个long long类型的无符号整数转化为字符串
                    if (sdsavail(s) < l) {///空间如果不够，需要申请新的内存空间
                        s = sdsMakeRoomFor(s,l);
                    }
                    memcpy(s+i,buf,l); ///将得到的字符串拷贝到s+i处
                    sdsinclen(s,l);///更新s的len
                    i += l;///移动i到i+l
                }
                break;
            default: ///处理%%和%<unknow>e类型的数据
                s[i++] = next;
                sdsinclen(s,1);
                break;
            }
            break;
        default:
            s[i++] = *f;
            sdsinclen(s,1);
            break;
        }
        f++;
    }
    va_end(ap);

    ///在字符串的末尾增加一个'\0'结尾标志符
    s[i] = '\0';
    return s;
}

/* 从左到右删除字符串中仅由cset中发现的相邻字符组成的部分，这是一个空终止('\0')的C字符串。
 *
 * 调用后，修改后的sds字符串不再有效，所有引用必须用调用返回的新指针替换。
 *
 * 例如:
 *
 * s = sdsnew("AA...AA.a.aa.aHelloWorld     :::");
 * s = sdstrim(s,"Aa. :");
 * printf("%s\n", s);
 *
 *输出结果: "HelloWorld".
 */
///该函数功能是在s中将cset中出现字符去除，最后得到新的字符串
sds sdstrim(sds s, const char *cset) {
    char *start, *end, *sp, *ep;
    size_t len;

    sp = start = s; ///获取s字符串开始的位置，将其赋值给sp和start
    ep = end = s+sdslen(s)-1; ///获取s的尾部，将其赋值给ep和end
    
    ///strchr(a, b)是库函数，它功能是在a中查找b第一次出现的地方，匹配成功后返回该地址
    while(sp <= end && strchr(cset, *sp)) sp++; ///从前向后匹配
    while(ep > sp && strchr(cset, *ep)) ep--; ///从后向前匹配
    len = (sp > ep) ? 0 : ((ep-sp)+1); ///新的字符串的长度，如果s中的所有字符都匹配到，则为0，否则为((ep-sp)+1
    if (s != sp) memmove(s, sp, len); ///将字符串进行规整，将空洞的buf[]进行规整，将它们移动到buf[]开头位置
    s[len] = '\0'; ///添上结尾标志符
    sdssetlen(s,len); ///更新s的len
    return s;
}

/* 将字符串转换为一个较小的(或相等的)字符串，只包含由'start'和'end'索引指定的子字符串。（范围截取函数）
 *
 * 开始和结束可以是负数，其中-1表示字符串的最后一个字符，-2表示倒数第二个字符，依此类推。
 *
 * 间隔是包含在内的，因此开始字符和结束字符将是结果字符串的一部分。
 *
 * 例子:
 *
 * s = sdsnew("Hello World");
 * sdsrange(s,1,-1); => "ello World"
 */
void sdsrange(sds s, ssize_t start, ssize_t end) {
    size_t newlen, len = sdslen(s);

    if (len == 0) return; ///如果愿字符串为空，直接返回
    if (start < 0) { ///如果start是负数，它可能是从s倒数start处来时
        start = len+start;
        if (start < 0) start = 0;///如果发现start还是小于0，表示它合法，将其置为0，从字符串开始处开始
    }
    if (end < 0) { ///如果end小于0 ，将其做和start相同的操作
        end = len+end;
        if (end < 0) end = 0;
    }
    newlen = (start > end) ? 0 : (end-start)+1; ///如果start > end，将返回空字符串，长度为0，否则长度为end-start + 1
    if (newlen != 0) { ///如果新字符串的长度不为0
        if (start >= (ssize_t)len) { /// 传入的start大于len，表示已经在buf[]未用部分，则内容为空，将其长度置为0
            newlen = 0;
        } else if (end >= (ssize_t)len) { ///如果end大于len，处于buf[]未用部分，需要更新end
            end = len-1;
            newlen = (start > end) ? 0 : (end-start)+1;
        }
    } else {
        start = 0;
    }
    if (start && newlen) memmove(s, s+start, newlen); ///将s中s + start到 s+newlen-1 部分的字符串拷贝s开始位置，
    s[newlen] = 0; ///结束标志符
    sdssetlen(s,newlen); ///更新s的len
}

///将sds中字符转化为小写
void sdstolower(sds s) {
    size_t len = sdslen(s), j;

    for (j = 0; j < len; j++) s[j] = tolower(s[j]);
}

///将sds中的字符转化为大写
void sdstoupper(sds s) {
    size_t len = sdslen(s), j;

    for (j = 0; j < len; j++) s[j] = toupper(s[j]);
}

/* 用memcmp()比较两个sds字符串s1和s2。
 *
 * 返回值:
 *
 *     正数 if s1 > s2.
 *     负数 if s1 < s2.
 *     0，如果s1和s2是完全相同的二进制字符串。
 *
 * 如果两个字符串共享完全相同的前缀，但其中一个有额外的字符，则认为较长的字符串大于较小的字符串。
 */
int sdscmp(const sds s1, const sds s2) {
    size_t l1, l2, minlen;
    int cmp;

    l1 = sdslen(s1);
    l2 = sdslen(s2);
    minlen = (l1 < l2) ? l1 : l2;
    cmp = memcmp(s1,s2,minlen);
    if (cmp == 0) return l1>l2? 1: (l1<l2? -1: 0);
    return cmp;
}

/* Split 's' with separator in 'sep'. An array
 * of sds strings is returned. *count will be set
 * by reference to the number of tokens returned.
 *
 * On out of memory, zero length string, zero length
 * separator, NULL is returned.
 *
 * Note that 'sep' is able to split a string using
 * a multi-character separator. For example
 * sdssplit("foo_-_bar","_-_"); will return two
 * elements "foo" and "bar".
 *
 * This version of the function is binary-safe but
 * requires length arguments. sdssplit() is just the
 * same function but for zero-terminated strings.
 */
///用sep字符串开切割字符串s，s的长度为len， sep的长度为splen，count用来记录切割后字符串的个数
sds *sdssplitlen(const char *s, ssize_t len, const char *sep, int seplen, int *count) {
    int elements = 0, slots = 5;
    long start = 0, j;
    sds *tokens; ///这是一个sds数组， 用来存储切割后的结果

    if (seplen < 1 || len < 0) return NULL; ///如果sep为空或者s不存在， 则直接返回

    tokens = s_malloc(sizeof(sds)*slots); ///申请内存空间，大小为sds长度的5倍
    if (tokens == NULL) return NULL;

    if (len == 0) {
        *count = 0;
        return tokens; ///len为0,则表示不进行切割，直接返回即可
    }
    for (j = 0; j < (len-(seplen-1)); j++) { ///判断sep在s中国是否存在，按照从左到右的字符串匹配规则，需要匹配len-(seplen - 1)次
        if (slots < elements+2) { ///确保有足够的空间内
            sds *newtokens;

            slots *= 2;
            newtokens = s_realloc(tokens,sizeof(sds)*slots); ///j进行内存扩展，
            if (newtokens == NULL) goto cleanup; ///如果扩展内存失败，则利用goto语句跳转到cleanup位置
            tokens = newtokens; ///相当于对tokens进行了一次扩容操作，大小为原来2倍
        }
        if ((seplen == 1 && *(s+j) == sep[0]) || (memcmp(s+j,sep,seplen) == 0)) { ///如果在s中找到了sep，这里表示sep可以是一个字符或者字符串
            tokens[elements] = sdsnewlen(s+start,j-start); ///将切割后的结果放到tokens数组中
            if (tokens[elements] == NULL) goto cleanup; ///如果创建字符串失败，用goto语句跳转到cleanup
            elements++; ///elemets游标向后移动
            start = j+seplen; ///调整start的位置
            j = j+seplen-1; /* skip the separator */
        }
    }
    
    ///需要将最后一个字符串添加到尾部，也就是tokens数组的最后位置
    tokens[elements] = sdsnewlen(s+start,len-start);
    if (tokens[elements] == NULL) goto cleanup;
    elements++;
    *count = elements; ///将elements的结构复制给count
    return tokens;

    ///看到这个标志，基本都会想到goto语句。
cleanup:
    {
        int i;
        for (i = 0; i < elements; i++) sdsfree(tokens[i]); ///释放token的空间
        s_free(tokens);
        *count = 0;
        return NULL;
    }
}

///释放sdssplitlen()返回的结果，或者“token”为空时不做任何事情。
void sdsfreesplitres(sds *tokens, int count) {
    if (!tokens) return;
    while(count--)
        sdsfree(tokens[count]);
    s_free(tokens);
}

/// 向sds字符串"s"追加一个转义字符串表示，其中所有不可打印字符(用isprint()测试)都转换为转义字符，形式为"\n\r\a…"或"\x<十六进制数字>"。
/// 调用后，修改后的sds字符串不再有效，所有引用必须用调用返回的新指针替换。
sds sdscatrepr(sds s, const char *p, size_t len) {
    s = sdscatlen(s,"\"",1);///在s尾部先追加一个 " 字符
    while(len--) { ///len还没有追加完时，需要进行下面的操作
        switch(*p) { ///通过p的kissing来判断
        case '\\':
        case '"':
            s = sdscatprintf(s,"\\%c",*p); ///如果要追加的字符是 " 或者 \ ， 直接在其尾部追加即可
            break;
        case '\n': s = sdscatlen(s,"\\n",2); break; ///注意："\\n"是两个字符，一个 \ 和一个 \n
        case '\r': s = sdscatlen(s,"\\r",2); break;
        case '\t': s = sdscatlen(s,"\\t",2); break;
        case '\a': s = sdscatlen(s,"\\a",2); break;
        case '\b': s = sdscatlen(s,"\\b",2); break;
        default:
            if (isprint(*p))
                s = sdscatprintf(s,"%c",*p);
            else
                s = sdscatprintf(s,"\\x%02x",(unsigned char)*p);
            break;
        }
        p++;
    }
    return sdscatlen(s,"\"",1); ///最后在尾部追加一个 “ 字符
}

///如果“c”是一个有效的十六进制数字，它将返回非零。
int is_hex_digit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

///用于sdssplitargs()的帮助函数，它将十六进制数字转换为0到15的整数
int hex_digit_to_int(char c) {
    switch(c) {
    case '0': return 0;
    case '1': return 1;
    case '2': return 2;
    case '3': return 3;
    case '4': return 4;
    case '5': return 5;
    case '6': return 6;
    case '7': return 7;
    case '8': return 8;
    case '9': return 9;
    case 'a': case 'A': return 10;
    case 'b': case 'B': return 11;
    case 'c': case 'C': return 12;
    case 'd': case 'D': return 13;
    case 'e': case 'E': return 14;
    case 'f': case 'F': return 15;
    default: return 0;
    }
}

/*将一行分割成参数，其中每个参数可以在以下编程语言复制类似的形式:
 * foo bar "newline are supported\n"和"\xff\x00otherstuff"
 * 参数的数量存储在*argc中，并返回一个sds数组。
 * 调用者应该使用sdsfreesplitres()释放生成的sds字符串数组。
 * 注意，sdscatrepr()可以将字符串转换回引用字符串，格式与sdssplitargs()可以解析的格式相同。
 * 如果成功，该函数将返回分配的标记，即使输入字符串为空，或NULL，如果输入包含不平衡引号或关闭引号后跟非空格字符，如"foo"bar或"foo'
 */
sds *sdssplitargs(const char *line, int *argc) {
    const char *p = line;
    char *current = NULL;
    char **vector = NULL;

    *argc = 0;
    while(1) {
        while(*p && isspace(*p)) p++; ///跳过空白行
        if (*p) { ///如果p满足条件
            /* get a token */
            int inq=0;  /* set to 1 if we are in "quotes" */
            int insq=0; /* set to 1 if we are in 'single quotes' */
            int done=0;

            if (current == NULL) current = sdsempty();
            while(!done) {
                if (inq) {
                    if (*p == '\\' && *(p+1) == 'x' &&
                                             is_hex_digit(*(p+2)) &&
                                             is_hex_digit(*(p+3)))
                    {
                        unsigned char byte;

                        byte = (hex_digit_to_int(*(p+2))*16)+
                                hex_digit_to_int(*(p+3));
                        current = sdscatlen(current,(char*)&byte,1);
                        p += 3;
                    } else if (*p == '\\' && *(p+1)) {
                        char c;

                        p++;
                        switch(*p) {
                        case 'n': c = '\n'; break;
                        case 'r': c = '\r'; break;
                        case 't': c = '\t'; break;
                        case 'b': c = '\b'; break;
                        case 'a': c = '\a'; break;
                        default: c = *p; break;
                        }
                        current = sdscatlen(current,&c,1);
                    } else if (*p == '"') {
                        /* closing quote must be followed by a space or
                         * nothing at all. */
                        if (*(p+1) && !isspace(*(p+1))) goto err;
                        done=1;
                    } else if (!*p) {
                        /* unterminated quotes */
                        goto err;
                    } else {
                        current = sdscatlen(current,p,1);
                    }
                } else if (insq) {
                    if (*p == '\\' && *(p+1) == '\'') {
                        p++;
                        current = sdscatlen(current,"'",1);
                    } else if (*p == '\'') {
                        /* closing quote must be followed by a space or
                         * nothing at all. */
                        if (*(p+1) && !isspace(*(p+1))) goto err;
                        done=1;
                    } else if (!*p) {
                        /* unterminated quotes */
                        goto err;
                    } else {
                        current = sdscatlen(current,p,1);
                    }
                } else {
                    switch(*p) {
                    case ' ':
                    case '\n':
                    case '\r':
                    case '\t':
                    case '\0':
                        done=1;
                        break;
                    case '"':
                        inq=1;
                        break;
                    case '\'':
                        insq=1;
                        break;
                    default:
                        current = sdscatlen(current,p,1);
                        break;
                    }
                }
                if (*p) p++;
            }
            /* add the token to the vector */
            vector = s_realloc(vector,((*argc)+1)*sizeof(char*));
            vector[*argc] = current;
            (*argc)++;
            current = NULL;
        } else {
            /* Even on empty input string return something not NULL. */
            if (vector == NULL) vector = s_malloc(sizeof(void*));
            return vector;
        }
    }

err:
    while((*argc)--)
        sdsfree(vector[*argc]);
    s_free(vector);
    if (current) sdsfree(current);
    *argc = 0;
    return NULL;
}

/* 修改字符串，将“from”字符串中指定的所有字符替换为“to”数组中对应的字符。
 *
 * 例如:sdsmapchars(mystring， "ho"， "01"， 2)
 * 的作用是将字符串“hello”转换为“0ell1”。
 *
 * 函数返回sds字符串指针，它总是与输入指针相同，因为不需要调整大小。
 */
sds sdsmapchars(sds s, const char *from, const char *to, size_t setlen) {
    size_t j, i, l = sdslen(s);

    for (j = 0; j < l; j++) { ///遍历字符串s
        for (i = 0; i < setlen; i++) { ///遍历字符串to
            if (s[j] == from[i]) { ///如果s中有字符和form中相等，就进行替换操作
                s[j] = to[i]; ///替换操作
                break;
            }
        }
    }
    return s;
}

/// 使用指定的分隔符(也是一个C字符串)连接一个C字符串数组。以sds字符串的形式返回结果。
sds sdsjoin(char **argv, int argc, char *sep) {
    sds join = sdsempty(); ///创建一个空的sds字符串
    int j;

    for (j = 0; j < argc; j++) {
        join = sdscat(join, argv[j]); ///将argv[j]追加到join的尾部
        if (j != argc-1) join = sdscat(join,sep); ///如果该字符串不是字符数组中最后一个，需要在jion追加一个分隔符
    }
    return join;
}

///与sdsjoin类似，但连接的是一个SDS字符串数组。
sds sdsjoinsds(sds *argv, int argc, const char *sep, size_t seplen) {
    sds join = sdsempty();
    int j;

    for (j = 0; j < argc; j++) {
        join = sdscatsds(join, argv[j]);
        if (j != argc-1) join = sdscatlen(join,sep,seplen);
    }
    return join;
}

/// 封装SDS使用的分配器。注意，SDS实际上只使用在sdsallo .h中定义的宏，以避免支付函数调用的开销。这里，我们仅为SDS链接到的程序定义这些包装器，如果它们希望触及SDS内部，即使它们使用不同的分配器
///空间分配
void *sds_malloc(size_t size) { return s_malloc(size); }

///重新分配空间
void *sds_realloc(void *ptr, size_t size) { return s_realloc(ptr,size); }

///释放空间
void sds_free(void *ptr) { s_free(ptr); }

#if defined(SDS_TEST_MAIN)
#include <stdio.h>
#include "testhelp.h"
#include "limits.h"

#define UNUSED(x) (void)(x)
int sdsTest(void) {
    {
        sds x = sdsnew("foo"), y;

        test_cond("Create a string and obtain the length",
            sdslen(x) == 3 && memcmp(x,"foo\0",4) == 0)

        sdsfree(x);
        x = sdsnewlen("foo",2);
        test_cond("Create a string with specified length",
            sdslen(x) == 2 && memcmp(x,"fo\0",3) == 0)

        x = sdscat(x,"bar");
        test_cond("Strings concatenation",
            sdslen(x) == 5 && memcmp(x,"fobar\0",6) == 0);

        x = sdscpy(x,"a");
        test_cond("sdscpy() against an originally longer string",
            sdslen(x) == 1 && memcmp(x,"a\0",2) == 0)

        x = sdscpy(x,"xyzxxxxxxxxxxyyyyyyyyyykkkkkkkkkk");
        test_cond("sdscpy() against an originally shorter string",
            sdslen(x) == 33 &&
            memcmp(x,"xyzxxxxxxxxxxyyyyyyyyyykkkkkkkkkk\0",33) == 0)

        sdsfree(x);
        x = sdscatprintf(sdsempty(),"%d",123);
        test_cond("sdscatprintf() seems working in the base case",
            sdslen(x) == 3 && memcmp(x,"123\0",4) == 0)

        sdsfree(x);
        x = sdsnew("--");
        x = sdscatfmt(x, "Hello %s World %I,%I--", "Hi!", LLONG_MIN,LLONG_MAX);
        test_cond("sdscatfmt() seems working in the base case",
            sdslen(x) == 60 &&
            memcmp(x,"--Hello Hi! World -9223372036854775808,"
                     "9223372036854775807--",60) == 0)
        printf("[%s]\n",x);

        sdsfree(x);
        x = sdsnew("--");
        x = sdscatfmt(x, "%u,%U--", UINT_MAX, ULLONG_MAX);
        test_cond("sdscatfmt() seems working with unsigned numbers",
            sdslen(x) == 35 &&
            memcmp(x,"--4294967295,18446744073709551615--",35) == 0)

        sdsfree(x);
        x = sdsnew(" x ");
        sdstrim(x," x");
        test_cond("sdstrim() works when all chars match",
            sdslen(x) == 0)

        sdsfree(x);
        x = sdsnew(" x ");
        sdstrim(x," ");
        test_cond("sdstrim() works when a single char remains",
            sdslen(x) == 1 && x[0] == 'x')

        sdsfree(x);
        x = sdsnew("xxciaoyyy");
        sdstrim(x,"xy");
        test_cond("sdstrim() correctly trims characters",
            sdslen(x) == 4 && memcmp(x,"ciao\0",5) == 0)

        y = sdsdup(x);
        sdsrange(y,1,1);
        test_cond("sdsrange(...,1,1)",
            sdslen(y) == 1 && memcmp(y,"i\0",2) == 0)

        sdsfree(y);
        y = sdsdup(x);
        sdsrange(y,1,-1);
        test_cond("sdsrange(...,1,-1)",
            sdslen(y) == 3 && memcmp(y,"iao\0",4) == 0)

        sdsfree(y);
        y = sdsdup(x);
        sdsrange(y,-2,-1);
        test_cond("sdsrange(...,-2,-1)",
            sdslen(y) == 2 && memcmp(y,"ao\0",3) == 0)

        sdsfree(y);
        y = sdsdup(x);
        sdsrange(y,2,1);
        test_cond("sdsrange(...,2,1)",
            sdslen(y) == 0 && memcmp(y,"\0",1) == 0)

        sdsfree(y);
        y = sdsdup(x);
        sdsrange(y,1,100);
        test_cond("sdsrange(...,1,100)",
            sdslen(y) == 3 && memcmp(y,"iao\0",4) == 0)

        sdsfree(y);
        y = sdsdup(x);
        sdsrange(y,100,100);
        test_cond("sdsrange(...,100,100)",
            sdslen(y) == 0 && memcmp(y,"\0",1) == 0)

        sdsfree(y);
        sdsfree(x);
        x = sdsnew("foo");
        y = sdsnew("foa");
        test_cond("sdscmp(foo,foa)", sdscmp(x,y) > 0)

        sdsfree(y);
        sdsfree(x);
        x = sdsnew("bar");
        y = sdsnew("bar");
        test_cond("sdscmp(bar,bar)", sdscmp(x,y) == 0)

        sdsfree(y);
        sdsfree(x);
        x = sdsnew("aar");
        y = sdsnew("bar");
        test_cond("sdscmp(bar,bar)", sdscmp(x,y) < 0)

        sdsfree(y);
        sdsfree(x);
        x = sdsnewlen("\a\n\0foo\r",7);
        y = sdscatrepr(sdsempty(),x,sdslen(x));
        test_cond("sdscatrepr(...data...)",
            memcmp(y,"\"\\a\\n\\x00foo\\r\"",15) == 0)

        {
            unsigned int oldfree;
            char *p;
            int step = 10, j, i;

            sdsfree(x);
            sdsfree(y);
            x = sdsnew("0");
            test_cond("sdsnew() free/len buffers", sdslen(x) == 1 && sdsavail(x) == 0);

            /* Run the test a few times in order to hit the first two
             * SDS header types. */
            for (i = 0; i < 10; i++) {
                int oldlen = sdslen(x);
                x = sdsMakeRoomFor(x,step);
                int type = x[-1]&SDS_TYPE_MASK;

                test_cond("sdsMakeRoomFor() len", sdslen(x) == oldlen);
                if (type != SDS_TYPE_5) {
                    test_cond("sdsMakeRoomFor() free", sdsavail(x) >= step);
                    oldfree = sdsavail(x);
                }
                p = x+oldlen;
                for (j = 0; j < step; j++) {
                    p[j] = 'A'+j;
                }
                sdsIncrLen(x,step);
            }
            test_cond("sdsMakeRoomFor() content",
                memcmp("0ABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDEFGHIJ",x,101) == 0);
            test_cond("sdsMakeRoomFor() final length",sdslen(x)==101);

            sdsfree(x);
        }
    }
    test_report()
    return 0;
}
#endif

#ifdef SDS_TEST_MAIN
int main(void) {
    return sdsTest();
}
#endif
