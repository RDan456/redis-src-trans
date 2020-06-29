#ifndef __SDS_H
#define __SDS_H

#define SDS_MAX_PREALLOC (1024*1024) ///预先分配内存的最大长度 1024 * 1024
extern const char *SDS_NOINIT;

#include <sys/types.h>
#include <stdarg.h>
#include <stdint.h>

typedef char *sds; ///语言类型的字符串，将它起了个别名为sds，所以sds具有c语言字符串的风格

/* Note: sdshdr5 is never used, we just access the flags byte directly.
 * However is here to document the layout of type 5 SDS strings. */
///注意:sdshdr5从未被使用过，我们只是直接访问标记字节。但是，在这里记录类型5 SDS字符串的布局。
struct __attribute__ ((__packed__)) sdshdr5 {
    unsigned char flags;
    char buf[];
};
struct __attribute__ ((__packed__)) sdshdr8 {
    uint8_t len; ///记录buff中已经占用的长度（也就是我们实际字符串的长度）
    uint8_t alloc; ///记录申请的空间，但是他的大小不包括头部和截止符。
    unsigned char flags; ///记录所属的type类型
    char buf[]; ///用来保存数据的实际数组
};
struct __attribute__ ((__packed__)) sdshdr16 {
    uint16_t len; /* used */
    uint16_t alloc; /* excluding the header and null terminator */
    unsigned char flags; /* 3 lsb of type, 5 unused bits */
    char buf[];
};
struct __attribute__ ((__packed__)) sdshdr32 {
    uint32_t len; /* used */
    uint32_t alloc; /* excluding the header and null terminator */
    unsigned char flags; /* 3 lsb of type, 5 unused bits */
    char buf[];
};
struct __attribute__ ((__packed__)) sdshdr64 {
    uint64_t len; /* used */
    uint64_t alloc; /* excluding the header and null terminator */
    unsigned char flags; /* 3 lsb of type, 5 unused bits */
    char buf[];
};

#define SDS_TYPE_5  0
#define SDS_TYPE_8  1
#define SDS_TYPE_16 2
#define SDS_TYPE_32 3
#define SDS_TYPE_64 4
#define SDS_TYPE_MASK 7
#define SDS_TYPE_BITS 3
#define SDS_HDR_VAR(T,s) struct sdshdr##T *sh = (void*)((s)-(sizeof(struct sdshdr##T)));
#define SDS_HDR(T,s) ((struct sdshdr##T *)((s)-(sizeof(struct sdshdr##T)))) ///获取sds头的位置，并将其返回
#define SDS_TYPE_5_LEN(f) ((f)>>SDS_TYPE_BITS)

///获取sds字符串的长度
static inline size_t sdslen(const sds s) {
    unsigned char flags = s[-1];  ///先获取到s的flags，通过flag判断它是f属于那种类型的结构体
    switch(flags&SDS_TYPE_MASK) { ///这是一个取余操作，它保证结构在0，1，2，3，4 这几个数中
        case SDS_TYPE_5: ///是sdshdr5类型，但是这种类型从来没有被使用
            return SDS_TYPE_5_LEN(flags);
        case SDS_TYPE_8:///是sdshdr8类型
            return SDS_HDR(8,s)->len; ///读取结构体的len的值
        case SDS_TYPE_16: ///sdshdr16
            return SDS_HDR(16,s)->len;
        case SDS_TYPE_32: ///sdshdr32
            return SDS_HDR(32,s)->len;
        case SDS_TYPE_64: ///sdshdr64
            return SDS_HDR(64,s)->len;
    }
    return 0;
}

///获取buf[]数组中未使用的空间，它的值等于alloc - len，获取的方式和上一个函数一样
static inline size_t sdsavail(const sds s) {
    unsigned char flags = s[-1];
    switch(flags&SDS_TYPE_MASK) {
        case SDS_TYPE_5: {
            return 0;
        }
        case SDS_TYPE_8: {
            SDS_HDR_VAR(8,s);
            return sh->alloc - sh->len;
        }
        case SDS_TYPE_16: {
            SDS_HDR_VAR(16,s);
            return sh->alloc - sh->len;
        }
        case SDS_TYPE_32: {
            SDS_HDR_VAR(32,s);
            return sh->alloc - sh->len;
        }
        case SDS_TYPE_64: {
            SDS_HDR_VAR(64,s);
            return sh->alloc - sh->len;
        }
    }
    return 0;
}

///设置字符串的长度，通过设置len的值进行设置，具体的数据操作方式见上上个函数
static inline void sdssetlen(sds s, size_t newlen) {
    unsigned char flags = s[-1];
    switch(flags&SDS_TYPE_MASK) {
        case SDS_TYPE_5:
            {
                unsigned char *fp = ((unsigned char*)s)-1;
                *fp = SDS_TYPE_5 | (newlen << SDS_TYPE_BITS);
            }
            break;
        case SDS_TYPE_8:
            SDS_HDR(8,s)->len = newlen;
            break;
        case SDS_TYPE_16:
            SDS_HDR(16,s)->len = newlen;
            break;
        case SDS_TYPE_32:
            SDS_HDR(32,s)->len = newlen;
            break;
        case SDS_TYPE_64:
            SDS_HDR(64,s)->len = newlen;
            break;
    }
}

///增加一个字符串的长度，在画原来的基础上加上inc的值
static inline void sdsinclen(sds s, size_t inc) {
    unsigned char flags = s[-1];
    switch(flags&SDS_TYPE_MASK) {
        case SDS_TYPE_5:
            {
                unsigned char *fp = ((unsigned char*)s)-1;
                unsigned char newlen = SDS_TYPE_5_LEN(flags)+inc;
                *fp = SDS_TYPE_5 | (newlen << SDS_TYPE_BITS);
            }
            break;
        case SDS_TYPE_8:
            SDS_HDR(8,s)->len += inc;
            break;
        case SDS_TYPE_16:
            SDS_HDR(16,s)->len += inc;
            break;
        case SDS_TYPE_32:
            SDS_HDR(32,s)->len += inc;
            break;
        case SDS_TYPE_64:
            SDS_HDR(64,s)->len += inc;
            break;
    }
}

/* sdsalloc() = sdsavail() + sdslen() */
///获取buf[]内存空间的大小，大小等于可以用空间大小加上字符串长度
static inline size_t sdsalloc(const sds s) {
    unsigned char flags = s[-1];
    switch(flags&SDS_TYPE_MASK) {
        case SDS_TYPE_5:
            return SDS_TYPE_5_LEN(flags);
        case SDS_TYPE_8:
            return SDS_HDR(8,s)->alloc;
        case SDS_TYPE_16:
            return SDS_HDR(16,s)->alloc;
        case SDS_TYPE_32:
            return SDS_HDR(32,s)->alloc;
        case SDS_TYPE_64:
            return SDS_HDR(64,s)->alloc;
    }
    return 0;
}

///设置buf[]空间大小的函数，通过修改alloc的值进行
static inline void sdssetalloc(sds s, size_t newlen) {
    unsigned char flags = s[-1];
    switch(flags&SDS_TYPE_MASK) {
        case SDS_TYPE_5:
            /* Nothing to do, this type has no total allocation info. */
            break;
        case SDS_TYPE_8:
            SDS_HDR(8,s)->alloc = newlen;
            break;
        case SDS_TYPE_16:
            SDS_HDR(16,s)->alloc = newlen;
            break;
        case SDS_TYPE_32:
            SDS_HDR(32,s)->alloc = newlen;
            break;
        case SDS_TYPE_64:
            SDS_HDR(64,s)->alloc = newlen;
            break;
    }
}

///我的总结：在这些sds操作函数中，new一般和新建字符串有关系，cat一般和字符串追加有关系，cpy一般和字符串拷贝有关系
sds sdsnewlen(const void *init, size_t initlen); ///创建一个新的字符串，它的大小为initlen，并且会将init字符串的值保存到这个新的字符串中
sds sdsnew(const char *init); ///创建一个默认长度的字符串
sds sdsempty(void); ///创建一个空的字符串，也就是它长度为0，但是会建立一个表头和结尾标志符'\0'。
sds sdsdup(const sds s);///复制字符串s
void sdsfree(sds s); ///释放s字符串的空间，
sds sdsgrowzero(sds s, size_t len); ///将字符串s的长度设置为0
sds sdscatlen(sds s, const void *t, size_t len); ///将字符串t追加到s后面，len为t字符串的大小
sds sdscat(sds s, const char *t); ///将t追加到s后面
sds sdscatsds(sds s, const sds t); ///将t追加到ts后面
sds sdscpylen(sds s, const char *t, size_t len); ///将t拷贝到s中，会将s原来的信息覆盖掉
sds sdscpy(sds s, const char *t); ///将t拷贝到s中，并将s的原来信息覆盖掉

sds sdscatvprintf(sds s, const char *fmt, va_list ap); ///打印函数，打印s的值
#ifdef __GNUC__
sds sdscatprintf(sds s, const char *fmt, ...) ///打印多个字符串，...表示可变参数
    __attribute__((format(printf, 2, 3)));
#else
sds sdscatprintf(sds s, const char *fmt, ...); ///打印多个字符串
#endif

sds sdscatfmt(sds s, char const *fmt, ...); ///多个字符串进行拼接操作
sds sdstrim(sds s, const char *cset); ///去重操作函数，在s中去除cset字符
void sdsrange(sds s, ssize_t start, ssize_t end); ///字符串截取函数，返回start到end之间的函数
void sdsupdatelen(sds s); ///更新字符串长度
void sdsclear(sds s); ///字符串重置保存空间
int sdscmp(const sds s1, const sds s2); ///比较两个字符串
sds *sdssplitlen(const char *s, ssize_t len, const char *sep, int seplen, int *count); ///字符串切割函数，
void sdsfreesplitres(sds *tokens, int count); ///释放tokend中的count个sds元素
void sdstolower(sds s);///将字符串中的所有字符转化为小写
void sdstoupper(sds s); ///将字符串中的所有字符都转化为大写
sds sdsfromlonglong(long long value); ///将一个long long类型的数字转化为sds类型的字符串
sds sdscatrepr(sds s, const char *p, size_t len); ///字符串p以“”的形式追花到字符串s的尾部
sds *sdssplitargs(const char *line, int *argc); ///对argc参数进行切分，主要是对config文件的分析
sds sdsmapchars(sds s, const char *from, const char *to, size_t setlen); ///将formt中的内容替换为to中的内容
sds sdsjoin(char **argv, int argc, char *sep); ///以分隔连接字符串子数组j构成的新字符串
sds sdsjoinsds(sds *argv, int argc, const char *sep, size_t seplen);

/* Low level functions exposed to the user API */
sds sdsMakeRoomFor(sds s, size_t addlen); ///对s进行扩展
void sdsIncrLen(sds s, ssize_t incr); ///增加字符串的长度
sds sdsRemoveFreeSpace(sds s); ///回收sds的buf[]中未使用的空间
size_t sdsAllocSize(sds s);///获取字符串s分配的空间的大小
void *sdsAllocPtr(sds s);///获取字符串s分配的空间


void *sds_malloc(size_t size); ///sds空间申请，大小为size
void *sds_realloc(void *ptr, size_t size); ///重新分配sds的内存空间
void sds_free(void *ptr); ///释放sds的内存空间

#ifdef REDIS_TEST
int sdsTest(int argc, char *argv[]);
#endif

#endif
