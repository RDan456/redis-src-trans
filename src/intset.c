#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "intset.h"
#include "zmalloc.h"
#include "endianconv.h"

/*请注意，这些编码是有序的, 因此有:
 * INTSET_ENC_INT16 < INTSET_ENC_INT32 < INTSET_ENC_INT64. */
#define INTSET_ENC_INT16 (sizeof(int16_t)) ///2个字节，表示的整数范围为-2^15 到 2^15 - 1
#define INTSET_ENC_INT32 (sizeof(int32_t)) ///4个字节，表示的整数范围为-2^31 到 2^31 - 1
#define INTSET_ENC_INT64 (sizeof(int64_t)) ///8个字节，表示的整数范围为-2^63 到 2^63 - 1

///返回v的编码格式
static uint8_t _intsetValueEncoding(int64_t v) {
    if (v < INT32_MIN || v > INT32_MAX)      ///如果超过了32位能表示的范围，就用64位表示
        return INTSET_ENC_INT64;
    else if (v < INT16_MIN || v > INT16_MAX) ///如果超过了16位能表示的范围，就用32位表示
        return INTSET_ENC_INT32;
    else                                     ///如果在16位能够表示的范围内，就用16位表示
        return INTSET_ENC_INT16;
}

///通过编码格式，返回集合中对应pos位置的元素
static int64_t _intsetGetEncoded(intset *is, int pos, uint8_t enc) {
    
    int64_t v64;
    int32_t v32;
    int16_t v16;
    if (enc == INTSET_ENC_INT64) { ///如果是64位的编码
        memcpy(&v64,((int64_t*)is->contents)+pos,sizeof(v64));///获取contents数组数组中pos元素
        memrev64ifbe(&v64); ///如果是大端序，就转化成小端序
        return v64; ///返回值
    } else if (enc == INTSET_ENC_INT32) { ///如果是32位的编码格式
        memcpy(&v32,((int32_t*)is->contents)+pos,sizeof(v32)); ///获取元素
        memrev32ifbe(&v32);///大小端转化
        return v32;
    } else { ///不是上面的两种编码格式，就是采用的16位的编码格式
        memcpy(&v16,((int16_t*)is->contents)+pos,sizeof(v16)); ///获取元素
        memrev16ifbe(&v16); ///大小端转化
        return v16; ///返回值
    }
}

///根据集合的编码格式，获取对应pos位置的元素，内部调用_intsetGetEncoded()方法实现
static int64_t _intsetGet(intset *is, int pos) {
    return _intsetGetEncoded(is,pos,intrev32ifbe(is->encoding));
}

///根据集合的编码格式，设置pos位置元素的value
static void _intsetSet(intset *is, int pos, int64_t value) {
   
    uint32_t encoding = intrev32ifbe(is->encoding); ///获取集合的编码格式
    if (encoding == INTSET_ENC_INT64) { ///如果是64位的编码
        ((int64_t*)is->contents)[pos] = value; ///设置pos位置的value
        memrev64ifbe(((int64_t*)is->contents)+pos); ///如果需要转化大小端
    } else if (encoding == INTSET_ENC_INT32) {///如果是32位的编码
        ((int32_t*)is->contents)[pos] = value; ///设置pos位置的编码
        memrev32ifbe(((int32_t*)is->contents)+pos); ///如果需要转化大小端
    } else { ///否则是16位的编码
        ((int16_t*)is->contents)[pos] = value; ///设置编码
        memrev16ifbe(((int16_t*)is->contents)+pos); ///如果需要大小端转化
    }
}

///创建一个新的的整数集合
intset *intsetNew(void) {
    intset *is = zmalloc(sizeof(intset)); ///申请内存空间
    is->encoding = intrev32ifbe(INTSET_ENC_INT16); ///设置编码格式，默认为16位编码
    is->length = 0; ///元素的个数
    return is;
}

///inset的大小伸缩
static intset *intsetResize(intset *is, uint32_t len) {
    
    uint32_t size = len*intrev32ifbe(is->encoding); ///计算需要扩展的空间
    is = zrealloc(is,sizeof(intset)+size); ///进行重新分配空间，如果新空间比原来空间大，原数组元素将会被保留
    return is;
}

///搜索“value”的位置。 找到值后返回1，并将“pos”设置为该值在整数集中的位置。 当值不存在于intset中时返回0，并将“ pos”设置为可以插入“value”的位置。
static uint8_t intsetSearch(intset *is, int64_t value, uint32_t *pos) {
   
    ///设置最小下标，最大下标，以及mid，看到这三个参数，应该想到折半查找..........
    int min = 0, max = intrev32ifbe(is->length)-1, mid = -1;
    int64_t cur = -1;

    if (intrev32ifbe(is->length) == 0) { ///如果集合是一个空集合
        if (pos) *pos = 0; ///那么可插入元素的位置为0
        return 0;
    } else { ///如果集合不为空
        if (value > _intsetGet(is,max)) { ///因为h集合是有序的，所以如果value大于最后一个元素
            if (pos) *pos = intrev32ifbe(is->length); ///pos设置集合尾
            return 0;
        } else if (value < _intsetGet(is,0)) { ///如果value小于集合第一个元素，表示也查不到
            if (pos) *pos = 0; ///可插入的位置位pos
            return 0;
        }
    }

    ///采用二分（折半）查找的方式查询数据
    while(max >= min) {
        mid = ((unsigned int)min + (unsigned int)max) >> 1; ///mid为(min + max)/2，这里采用移位操作，效率更高
        cur = _intsetGet(is,mid); ///获取mid位置的元素
        if (value > cur) { ///如果value大于cur，就要去后半部分查找
            min = mid+1; ///修改min
        } else if (value < cur) { ///如果value小于cur，就要去前面部分查找
            max = mid-1; ///修改Max
        } else { ///否则，表示查询到元素，在mid位置
            break;
        }
    }
    if (value == cur) { ///如果val == cur
        if (pos) *pos = mid; ///返回位置
        return 1;
    } else { ///否则，表示元素在集合中不存在
        if (pos) *pos = min; ///可在pos位置插入value
        return 0;
    }
}

///将整数升级为更大的编码，并插入给定的整数。
static intset *intsetUpgradeAndAdd(intset *is, int64_t value) {
    
    uint8_t curenc = intrev32ifbe(is->encoding); ///获取集合当前的编码格式
    uint8_t newenc = _intsetValueEncoding(value); ///获取新的合适的编码格式
    int length = intrev32ifbe(is->length); ///获取集合中元素的数量
    int prepend = value < 0 ? 1 : 0;  ///如果value小于0，则要将这个元素添加数组开始的位置，需要移动元素

    is->encoding = intrev32ifbe(newenc); ///更新集合的编码格式
    is = intsetResize(is,intrev32ifbe(is->length)+1); ///更新集合的元素个数

    ///从头到尾升级，因此我们不会覆盖值。
    ///会多一个空格，用来保存value这个新的元素
    while(length--)
        _intsetSet(is,length+prepend,_intsetGetEncoded(is,length,curenc));

    ///根据prepend来确定是在集合的哪个位置插入value
    if (prepend) ///如果prepend = 1
        _intsetSet(is,0,value); ///在pos = 0处插入元素
    else ///prepend = 0
        _intsetSet(is,intrev32ifbe(is->length),value); ///在数组尾部插入一个元素
    is->length = intrev32ifbe(intrev32ifbe(is->length)+1); ///更新集合中元素个数
    return is;
}

///移动元素，将从from开始的元素到集合最后一个元素这个闭区间的元素移动到to开始的地址空间
static void intsetMoveTail(intset *is, uint32_t from, uint32_t to) {
    
    void *src, *dst;
    uint32_t bytes = intrev32ifbe(is->length)-from; ///获取要移动的元素个数
    uint32_t encoding = intrev32ifbe(is->encoding); ///获取集合的编码格式

    if (encoding == INTSET_ENC_INT64) { ///如果是64位编码
        src = (int64_t*)is->contents+from; ///获取开始移动的第一个元素的位置
        dst = (int64_t*)is->contents+to; ///获取移动元素的目标地址
        bytes *= sizeof(int64_t); ///如果要移动的地址空间长度
    } else if (encoding == INTSET_ENC_INT32) { ///如果是32位编码
        src = (int32_t*)is->contents+from;
        dst = (int32_t*)is->contents+to;
        bytes *= sizeof(int32_t);
    } else {    ///否则就是16位编码
        src = (int16_t*)is->contents+from;
        dst = (int16_t*)is->contents+to;
        bytes *= sizeof(int16_t);
    }
    memmove(dst,src,bytes); ///将要拷贝的地址拷贝到目标地址
}

///将value插入到集合中，如果插入成功，saccess的值为1，否则success的值为0
intset *intsetAdd(intset *is, int64_t value, uint8_t *success) {
    
    uint8_t valenc = _intsetValueEncoding(value); ///获取适合value的编码格式
    uint32_t pos; ///插入的位置
    if (success) *success = 1; ///如果success不为空，默认success为z1

    /* Upgrade encoding if necessary. If we need to upgrade, we know that
     * this value should be either appended (if > 0) or prepended (if < 0),
     * because it lies outside the range of existing values. */
    ///如果value合适的编码要比集合的编码大
    if (valenc > intrev32ifbe(is->encoding)) {
        
        return intsetUpgradeAndAdd(is,value); ///需要对集合编码升级，并增加value
    } else { ///如果value合适的编码小于或者等于集合的编码
        /* Abort if the value is already present in the set.
         * This call will populate "pos" with the right position to insert
         * the value when it cannot be found. */
        if (intsetSearch(is,value,&pos)) { ///查找到适合value的插入的位置，
            if (success) *success = 0; ///如果value在集合中已经存在了，success为0
            return is; ///返回集合
        }

        is = intsetResize(is,intrev32ifbe(is->length)+1); ///对集合进行扩容操作
        ///如果插入的位置在中间，就将pos及其以后的元素移动到pos+1及其以后
        if (pos < intrev32ifbe(is->length)) intsetMoveTail(is,pos,pos+1);
    }

    _intsetSet(is,pos,value); ///在pos位置插入元素
    is->length = intrev32ifbe(intrev32ifbe(is->length)+1); ///更新集合的元素个数
    return is;
}

///从集合中删除value，成功success为1，否则为0
intset *intsetRemove(intset *is, int64_t value, int *success) {
    
    uint8_t valenc = _intsetValueEncoding(value); ///找到适合value的编码
    uint32_t pos;
    if (success) *success = 0; ///success默认为0
    ///如果适合value的编码小于d或等于集合编码，在集合中有找到value这个元素
    if (valenc <= intrev32ifbe(is->encoding) && intsetSearch(is,value,&pos)) {
        uint32_t len = intrev32ifbe(is->length); ///获取集合中元素个数

        if (success) *success = 1;  ///可以进行元素删除

        if (pos < (len-1)) intsetMoveTail(is,pos+1,pos); ///对pos位置的元素用pos+1位置元素覆盖
        is = intsetResize(is,len-1); ///对集合的数组进行缩容操作
        is->length = intrev32ifbe(len-1); ///更新集合的元素的个数
    }
    return is; ///返回集合
}

///查询value是否在集合中，如果存在返回1，否则返回0
uint8_t intsetFind(intset *is, int64_t value) {
    
    uint8_t valenc = _intsetValueEncoding(value); ///获取适合value的编码
    ///如果适合value的编码小于或者等于集合的编码，并且在集合中有查询到元素，返回1， 否则为0
    return valenc <= intrev32ifbe(is->encoding) && intsetSearch(is,value,NULL);
}

///随机返回一个集合元素
int64_t intsetRandom(intset *is) {
    
    ///rand()%intrev32ifbe(is->length)作用是获取一个0到is->length-1闭区间的随机数
    return _intsetGet(is,rand()%intrev32ifbe(is->length));
}

///获取给定位置的值。 当此位置超出范围时，该函数返回0，而在范围之外时，该函数返回1。
uint8_t intsetGet(intset *is, uint32_t pos, int64_t *value) {
    
    if (pos < intrev32ifbe(is->length)) {
        *value = _intsetGet(is,pos); ///返回pos，保存在value中
        return 1;
    }
    return 0;
}

///获取集合的元素个数
uint32_t intsetLen(const intset *is) {
    return intrev32ifbe(is->length);
}

///获取集合的字节总数
size_t intsetBlobLen(intset *is) {
    return sizeof(intset)+intrev32ifbe(is->length)*intrev32ifbe(is->encoding);
}

#ifdef REDIS_TEST
#include <sys/time.h>
#include <time.h>

#if 0
static void intsetRepr(intset *is) {
    for (uint32_t i = 0; i < intrev32ifbe(is->length); i++) {
        printf("%lld\n", (uint64_t)_intsetGet(is,i));
    }
    printf("\n");
}

static void error(char *err) {
    printf("%s\n", err);
    exit(1);
}
#endif

static void ok(void) {
    printf("OK\n");
}

static long long usec(void) {
    struct timeval tv;
    gettimeofday(&tv,NULL);
    return (((long long)tv.tv_sec)*1000000)+tv.tv_usec;
}

#define assert(_e) ((_e)?(void)0:(_assert(#_e,__FILE__,__LINE__),exit(1)))
static void _assert(char *estr, char *file, int line) {
    printf("\n\n=== ASSERTION FAILED ===\n");
    printf("==> %s:%d '%s' is not true\n",file,line,estr);
}

static intset *createSet(int bits, int size) {
    uint64_t mask = (1<<bits)-1;
    uint64_t value;
    intset *is = intsetNew();

    for (int i = 0; i < size; i++) {
        if (bits > 32) {
            value = (rand()*rand()) & mask;
        } else {
            value = rand() & mask;
        }
        is = intsetAdd(is,value,NULL);
    }
    return is;
}

static void checkConsistency(intset *is) {
    for (uint32_t i = 0; i < (intrev32ifbe(is->length)-1); i++) {
        uint32_t encoding = intrev32ifbe(is->encoding);

        if (encoding == INTSET_ENC_INT16) {
            int16_t *i16 = (int16_t*)is->contents;
            assert(i16[i] < i16[i+1]);
        } else if (encoding == INTSET_ENC_INT32) {
            int32_t *i32 = (int32_t*)is->contents;
            assert(i32[i] < i32[i+1]);
        } else {
            int64_t *i64 = (int64_t*)is->contents;
            assert(i64[i] < i64[i+1]);
        }
    }
}

#define UNUSED(x) (void)(x)
int intsetTest(int argc, char **argv) {
    uint8_t success;
    int i;
    intset *is;
    srand(time(NULL));

    UNUSED(argc);
    UNUSED(argv);

    printf("Value encodings: "); {
        assert(_intsetValueEncoding(-32768) == INTSET_ENC_INT16);
        assert(_intsetValueEncoding(+32767) == INTSET_ENC_INT16);
        assert(_intsetValueEncoding(-32769) == INTSET_ENC_INT32);
        assert(_intsetValueEncoding(+32768) == INTSET_ENC_INT32);
        assert(_intsetValueEncoding(-2147483648) == INTSET_ENC_INT32);
        assert(_intsetValueEncoding(+2147483647) == INTSET_ENC_INT32);
        assert(_intsetValueEncoding(-2147483649) == INTSET_ENC_INT64);
        assert(_intsetValueEncoding(+2147483648) == INTSET_ENC_INT64);
        assert(_intsetValueEncoding(-9223372036854775808ull) ==
                    INTSET_ENC_INT64);
        assert(_intsetValueEncoding(+9223372036854775807ull) ==
                    INTSET_ENC_INT64);
        ok();
    }

    printf("Basic adding: "); {
        is = intsetNew();
        is = intsetAdd(is,5,&success); assert(success);
        is = intsetAdd(is,6,&success); assert(success);
        is = intsetAdd(is,4,&success); assert(success);
        is = intsetAdd(is,4,&success); assert(!success);
        ok();
    }

    printf("Large number of random adds: "); {
        uint32_t inserts = 0;
        is = intsetNew();
        for (i = 0; i < 1024; i++) {
            is = intsetAdd(is,rand()%0x800,&success);
            if (success) inserts++;
        }
        assert(intrev32ifbe(is->length) == inserts);
        checkConsistency(is);
        ok();
    }

    printf("Upgrade from int16 to int32: "); {
        is = intsetNew();
        is = intsetAdd(is,32,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT16);
        is = intsetAdd(is,65535,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT32);
        assert(intsetFind(is,32));
        assert(intsetFind(is,65535));
        checkConsistency(is);

        is = intsetNew();
        is = intsetAdd(is,32,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT16);
        is = intsetAdd(is,-65535,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT32);
        assert(intsetFind(is,32));
        assert(intsetFind(is,-65535));
        checkConsistency(is);
        ok();
    }

    printf("Upgrade from int16 to int64: "); {
        is = intsetNew();
        is = intsetAdd(is,32,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT16);
        is = intsetAdd(is,4294967295,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT64);
        assert(intsetFind(is,32));
        assert(intsetFind(is,4294967295));
        checkConsistency(is);

        is = intsetNew();
        is = intsetAdd(is,32,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT16);
        is = intsetAdd(is,-4294967295,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT64);
        assert(intsetFind(is,32));
        assert(intsetFind(is,-4294967295));
        checkConsistency(is);
        ok();
    }

    printf("Upgrade from int32 to int64: "); {
        is = intsetNew();
        is = intsetAdd(is,65535,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT32);
        is = intsetAdd(is,4294967295,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT64);
        assert(intsetFind(is,65535));
        assert(intsetFind(is,4294967295));
        checkConsistency(is);

        is = intsetNew();
        is = intsetAdd(is,65535,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT32);
        is = intsetAdd(is,-4294967295,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT64);
        assert(intsetFind(is,65535));
        assert(intsetFind(is,-4294967295));
        checkConsistency(is);
        ok();
    }

    printf("Stress lookups: "); {
        long num = 100000, size = 10000;
        int i, bits = 20;
        long long start;
        is = createSet(bits,size);
        checkConsistency(is);

        start = usec();
        for (i = 0; i < num; i++) intsetSearch(is,rand() % ((1<<bits)-1),NULL);
        printf("%ld lookups, %ld element set, %lldusec\n",
               num,size,usec()-start);
    }

    printf("Stress add+delete: "); {
        int i, v1, v2;
        is = intsetNew();
        for (i = 0; i < 0xffff; i++) {
            v1 = rand() % 0xfff;
            is = intsetAdd(is,v1,NULL);
            assert(intsetFind(is,v1));

            v2 = rand() % 0xfff;
            is = intsetRemove(is,v2,NULL);
            assert(!intsetFind(is,v2));
        }
        checkConsistency(is);
        ok();
    }

    return 0;
}
#endif
