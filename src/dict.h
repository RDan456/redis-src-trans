/*
 * redis中字典（dict）操作的定义
 */
#include <stdint.h>

#ifndef __DICT_H
#define __DICT_H

#define DICT_OK 0  ///
#define DICT_ERR 1 ///

/* Unused arguments generate annoying warnings... */
#define DICT_NOTUSED(V) ((void) V)

///hash表节点数据结构定义
typedef struct dictEntry {
    void *key; ///字典的key值
    union { ///定义value的共用体，他可以是一个指针、uint64、int64、double
        void *val;
        uint64_t u64;
        int64_t s64;
        double d;
    } v;
    struct dictEntry *next; ///该节点的下一个节点（dict采用拉链法hash冲突）
} dictEntry;

///hash表节点的一些操作，这些操作定义为函数指针的形式
typedef struct dictType {
    uint64_t (*hashFunction)(const void *key); ///函数指针，用来计算key对应的hash值
    void *(*keyDup)(void *privdata, const void *key); ///函数指针，用来复制key
    void *(*valDup)(void *privdata, const void *obj); ///函数指针，用来复制value
    int (*keyCompare)(void *privdata, const void *key1, const void *key2); ///函数指针，用来比较两个key是否相同
    void (*keyDestructor)(void *privdata, void *key); ///函数指针，用来释放key
    void (*valDestructor)(void *privdata, void *obj); ///函数指针，用来释放value
} dictType;

/// 这是我们的哈希表结构。 对于我们的旧表到新表，在实现增量重新哈希处理时，每个字典都有两个。
/// 这个是hash表定义的数据结构
typedef struct dictht {
    dictEntry **table; ///用来保存dictEntry的数组
    unsigned long size;  ///这个数组的长度
    unsigned long sizemask; ///这个是用来计算key在数组中的索引，它的值为size -1
    unsigned long used; ///用来记录dict中节点的数量
} dictht;

///redis中dict数据结构的的定义
typedef struct dict {
    dictType *type; ///指向dictType的指针，这里面包含着对节点的各种操作
    void *privdata; ///私有数据，保存着dictType结构中函数的参数
    dictht ht[2];  ///一个dict中有两张哈希表
    long rehashidx; ///如果rehashidx == -1，则没有进行重新哈希
    unsigned long iterators;///当前正在运行的迭代器数
} dict;

/* 如果将safe设置为1，则这是一个安全的迭代器，这意味着，即使在迭代时，也可以针对字典调用dictAdd，dictFind和其他函数。
 * 否则，它是不安全的迭代器，并且在迭代时仅应调用dictNext（）。
 */
typedef struct dictIterator {
    dict *d; ///被迭代的dict
    long index; ///当前遍历dict中hash表中的数组下标
    int table, safe; //table表示迭代的hash表，就是dict[0]、dict[1]中的一个，safe表示这个迭代器是否为安全的
    dictEntry *entry, *nextEntry; ///entry 表示iterator指向的当前节点，nextEntity为当前节点的下一个节点
    long long fingerprint;///不安全的迭代器指纹，用于滥用检测。
} dictIterator;

typedef void (dictScanFunction)(void *privdata, const dictEntry *de); ///字典扫描节点的方法
typedef void (dictScanBucketFunction)(void *privdata, dictEntry **bucketref); ///字典扫描hash桶的方法

///hash表中，数组的初始化大小，默认为4
#define DICT_HT_INITIAL_SIZE     4

///一系列的宏定义操作
#define dictFreeVal(d, entry) \    ///释放hash表中的节点值
    if ((d)->type->valDestructor) \ ///如果hash表中有定义节点值的销毁函数
        (d)->type->valDestructor((d)->privdata, (entry)->v.val) /// 就调用这个销毁函数进行节点值的销毁

#define dictSetVal(d, entry, _val_) do { \ //给hash表中的节点entry设置一个新值_val_
    if ((d)->type->valDup) \ ///如果hash中定义了函数节点值的复制函数
        (entry)->v.val = (d)->type->valDup((d)->privdata, _val_); \ ///通过valDup函数进行复制
    else \
        (entry)->v.val = (_val_); \ ///如果没有定义，就直接进行复制
} while(0)

#define dictSetSignedIntegerVal(entry, _val_) \ ///给节点设置有符号的整数值
    do { (entry)->v.s64 = _val_; } while(0)

#define dictSetUnsignedIntegerVal(entry, _val_) \ ///给节点设置无符号的整数值
    do { (entry)->v.u64 = _val_; } while(0)

#define dictSetDoubleVal(entry, _val_) \ ///给节点设置double类型的值
    do { (entry)->v.d = _val_; } while(0)

#define dictFreeKey(d, entry) \ ///释放key
    if ((d)->type->keyDestructor) \ ///如果hash表有定义keyDestructor， 就调用这个函数进行key值释放
        (d)->type->keyDestructor((d)->privdata, (entry)->key)

#define dictSetKey(d, entry, _key_) do { \ ///给节点设置key
    if ((d)->type->keyDup) \ ///如果hash有定义keyDup函数，就直接调用这个函数进行设置
        (entry)->key = (d)->type->keyDup((d)->privdata, _key_); \
    else \
        (entry)->key = (_key_); \ ///如果没有定义这个函数，就直接执行赋值操作
} while(0)

#define dictCompareKeys(d, key1, key2) \ ///比较两个key ：key1、key2是否相等
    (((d)->type->keyCompare) ? \ ///如果hashb表中有定义keyCompare这个函数，则直接调用这个函数，否则，直接比较两个key是否相等
        (d)->type->keyCompare((d)->privdata, key1, key2) : \
        (key1) == (key2))

#define dictHashKey(d, key) (d)->type->hashFunction(key) ///定义计算key的hash值的函数
#define dictGetKey(he) ((he)->key) ///定义获取节点key的函数
#define dictGetVal(he) ((he)->v.val) ///定义获取节点value的函数
#define dictGetSignedIntegerVal(he) ((he)->v.s64) ///定义获取有符号整数值的方法
#define dictGetUnsignedIntegerVal(he) ((he)->v.u64) ///定义获取无符号整数的方法
#define dictGetDoubleVal(he) ((he)->v.d) ///获取获取double值的方法
#define dictSlots(d) ((d)->ht[0].size+(d)->ht[1].size) ///定义获取dict中hash数组大小的方法
#define dictSize(d) ((d)->ht[0].used+(d)->ht[1].used) ///定义获取dict中元素个数的方法
#define dictIsRehashing(d) ((d)->rehashidx != -1) ///定义获取是正在进行rehash操作的方法

/* API 定义*/
dict *dictCreate(dictType *type, void *privDataPtr); ///构建一个新的dict
int dictExpand(dict *d, unsigned long size); ///根据size的大小调整字典d的hash表的大小
int dictAdd(dict *d, void *key, void *val); ///向字典中新添加一个键值对
dictEntry *dictAddRaw(dict *d, void *key, dictEntry **existing); ///将key插入到字典d新创建的节点上
dictEntry *dictAddOrFind(dict *d, void *key); ///查找key是否在字典表中存在，如果不存在就添加，存在就返回该节点
int dictReplace(dict *d, void *key, void *val); ///在字典中替换掉键为key的值
int dictDelete(dict *d, const void *key); ///从字典d中删除键为key的节点
dictEntry *dictUnlink(dict *ht, const void *key); ///
void dictFreeUnlinkedEntry(dict *d, dictEntry *he); ///
void dictRelease(dict *d); ///释放字典dict
dictEntry * dictFind(dict *d, const void *key);
void *dictFetchValue(dict *d, const void *key);
int dictResize(dict *d);
dictIterator *dictGetIterator(dict *d);
dictIterator *dictGetSafeIterator(dict *d);
dictEntry *dictNext(dictIterator *iter);
void dictReleaseIterator(dictIterator *iter);
dictEntry *dictGetRandomKey(dict *d);
dictEntry *dictGetFairRandomKey(dict *d);
unsigned int dictGetSomeKeys(dict *d, dictEntry **des, unsigned int count);
void dictGetStats(char *buf, size_t bufsize, dict *d);
uint64_t dictGenHashFunction(const void *key, int len);
uint64_t dictGenCaseHashFunction(const unsigned char *buf, int len);
void dictEmpty(dict *d, void(callback)(void*));
void dictEnableResize(void);
void dictDisableResize(void);
int dictRehash(dict *d, int n);
int dictRehashMilliseconds(dict *d, int ms);
void dictSetHashFunctionSeed(uint8_t *seed);
uint8_t *dictGetHashFunctionSeed(void);
unsigned long dictScan(dict *d, unsigned long v, dictScanFunction *fn, dictScanBucketFunction *bucketfn, void *privdata);
uint64_t dictGetHash(dict *d, const void *key); ///通过key在dict中获取值对应值为uint64
dictEntry **dictFindEntryRefByPtrAndHash(dict *d, const void *oldptr, uint64_t hash); ///

///hash表的类型，这三个函数在dict.c文件中有定义
extern dictType dictTypeHeapStringCopyKey;
extern dictType dictTypeHeapStrings;
extern dictType dictTypeHeapStringCopyKeyValue;

#endif /* __DICT_H */
