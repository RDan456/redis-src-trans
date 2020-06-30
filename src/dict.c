/*
 * redis中字典操作的实现
 */
#include "fmacros.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>
#include <sys/time.h>

#include "dict.h"
#include "zmalloc.h"
#ifndef DICT_BENCHMARK_MAIN
#include "redisassert.h"
#else
#include <assert.h>
#endif

/* 使用dictEnableResize()或者dictDisableResize()，我们可以根据需要启用/禁用哈希表的大小调整。 
 * 这对于Redis非常重要，因为我们使用写时复制，并且在有子执行保存操作时不希望在内存中移动太多。
 *
 * 请注意，即使将dict_can_resize设置为0，也不会阻止所有调整大小：如果元素数和存储桶数之比> dict_force_resize_ratio，则仍然允许散列表增大。 
 */
static int dict_can_resize = 1; ///表示字典是否启动rehash。dictEableResize()和dictDisableResize()可以修改该变量的值
static unsigned int dict_force_resize_ratio = 5;///如果used / size 大于了dict_force_resize_ratio，就需要强制进行rehash操作

///一些私有的方法
static int _dictExpandIfNeeded(dict *ht); ///将字典ht进行扩展操作
static unsigned long _dictNextPower(unsigned long size);  ///用来表示一个hash表数组大小的值，它为2的n次方并且大于size
static long _dictKeyIndex(dict *ht, const void *key, uint64_t hash, dictEntry **existing); ///获取key对应的hash表中的索引
static int _dictInit(dict *ht, dictType *type, void *privDataPtr); ///初始化hash表

/***************************begin : 一系列关于hash的函数***********************************/
static uint8_t dict_hash_function_seed[16];

void dictSetHashFunctionSeed(uint8_t *seed) {
    memcpy(dict_hash_function_seed,seed,sizeof(dict_hash_function_seed));
}

uint8_t *dictGetHashFunctionSeed(void) {
    return dict_hash_function_seed;
}

/* The default hashing function uses SipHash implementation
 * in siphash.c. */

uint64_t siphash(const uint8_t *in, const size_t inlen, const uint8_t *k);
uint64_t siphash_nocase(const uint8_t *in, const size_t inlen, const uint8_t *k);

uint64_t dictGenHashFunction(const void *key, int len) {
    return siphash(key,len,dict_hash_function_seed);
}

uint64_t dictGenCaseHashFunction(const unsigned char *buf, int len) {
    return siphash_nocase(buf,len,dict_hash_function_seed);
}
/***************************end : 一系列关于hash的函数***********************************/


/* 置已经用ht_init（）初始化的哈希表。
 * 意：此函数只能由ht_destroy（）调用。 
 */
static void _dictReset(dictht *ht)
{
    ht->table = NULL; ///将hash表中的数组置空 
    ht->size = 0;  ///将数组大小变为0
    ht->sizemask = 0; /// 将偏移量置为零
    ht->used = 0; ///将整个hash表中的元素个数置为零
}

///创建一个新的hash表
dict *dictCreate(dictType *type, void *privDataPtr)
{
    dict *d = zmalloc(sizeof(*d)); ///申请内存

    _dictInit(d,type,privDataPtr); ///调用_dictInit函数进行初始化
    return d;
}

///初始化hash表的函数
int _dictInit(dict *d, dictType *type, oid *privDataPtr)
{
    _dictReset(&d->ht[0]); ///调用_dictReset对字典中的两个hash表进行初始化
    _dictReset(&d->ht[1]);
    d->type = type; ///制定字典中节点操作的函数指针
    d->privdata = privDataPtr; ///私有数据的指针
    d->rehashidx = -1; ///设置是否正在进行rehash操作，-1表示没有进行
    d->iterators = 0; ///设置正在迭代的迭代器的数量
    return DICT_OK; ///返回操作成功
}

///将表的大小调整为包含所有元素的最小大小，但USED/BUCKETS比率的不变性接近<= 1 * 
int dictResize(dict *d)
{
    unsigned long minimal;
   
    /*如果有以下两种情况，表示不能进行resize操作：
     *1. dict_can_resize被设置为0表示不能进行resize操作，返回DICT_ERR，
     *2. 如果正在进行rehash操作，也不能进行resize操作，返回DICT_ERR
     */
    if (!dict_can_resize || dictIsRehashing(d)) return DICT_ERR; 
    minimal = d->ht[0].used; ///如果没有进行rehash操作，所有的数据存储在ht[0]这个hash表中，所以ht[0].used表示整个字典中的元素个数
    if (minimal < DICT_HT_INITIAL_SIZE) ///如果元素个数小于字典中hash表的初始化大小4，就直接将其赋值为4
        minimal = DICT_HT_INITIAL_SIZE;
    return dictExpand(d, minimal); ///对字典d进行resize操作
}

///扩展或创建哈希表
int dictExpand(dict *d, unsigned long size)
{
    ///如果字典正在进行rehashing操作或者传入的大小小于哈希表中已经存在的元素数，则大小无效，直接返回DICT_ERR
    if (dictIsRehashing(d) || d->ht[0].used > size)
        return DICT_ERR;

    dictht n; ///定义一个新的hash表
    unsigned long realsize = _dictNextPower(size); ///计算出适合的扩容的大小（2的指数）

    ///如果计算出realsize大小刚好和字典中的元素个数相等，就直接返回DICT_ERR
    if (realsize == d->ht[0].size) return DICT_ERR;

    ///初始化这个新的hash表的各个成员数据
    n.size = realsize;
    n.sizemask = realsize-1;
    n.table = zcalloc(realsize*sizeof(dictEntry*)); ///为这个hash表中的数组申请内存空间
    n.used = 0;

    ///如果是第一次hash进行hash操作，表示ht[0]里面没有任何元素，我们就直接将字典的ht[0]指向n
    if (d->ht[0].table == NULL) {
        d->ht[0] = n;
        return DICT_OK; ///返回操作成功
    }

    d->ht[1] = n; ///如果不是第一次hash操作，我们只能将字典的ht[1]指向n
    d->rehashidx = 0;///并且设置rehashidx，表示正在进行rehash操作
    return DICT_OK; /// 返回操作成功
}

/* 执行N步渐步式哈希。 如果仍有从ht[0]移动到ht[1]的键，则返回1，否则返回0。
 *
 * 请注意，每次进行rehash操作是以一个hash表的索引为单位，也就是一个数组中对应的hash桶，这个hash有的元素很多，有的可能没有元素。
 * 因此无法保证该函数在单个hash桶中也将重新哈希，因为它将总共访问最多N*10个空存储桶，否则它的工作量将不受限制，并且该函数可能会长时间阻塞。
 */
int dictRehash(dict *d, int n) {
	
    int empty_visits = n*10; ///最多访问的hash桶数量
    if (!dictIsRehashing(d)) return 0; ///如果此时没有进行rehash操作，直接返回0.

	///n步渐式hash，分n步进行
    while(n-- && d->ht[0].used != 0) { ///如果还在n步范围内，并且原hash表中还有元素，也就是ht[0]中的used不为0，则可以进行下面操作
        dictEntry *de, *nextde;

        /// 请注意rehashidx不会溢出，因为我们确定还有更多元素，因为ht[0].used！= 0 
        assert(d->ht[0].size > (unsigned long)d->rehashidx);
        while(d->ht[0].table[d->rehashidx] == NULL) {
            d->rehashidx++; 
            if (--empty_visits == 0) return 1; ///如果hash桶访问完了，就直接返回了
        }
        de = d->ht[0].table[d->rehashidx]; ///获取ht[0]中hash表中下标为d->rehashinx出的元素
       ///将该处hash桶中的元素移动到新的hash表中
        while(de) {
            uint64_t h;

            nextde = de->next; ///获取该元素的下一个元素
            h = dictHashKey(d, de->key) & d->ht[1].sizemask; ///获取该元素在新的hash中数组的位置，求出其下标
            de->next = d->ht[1].table[h]; ///采用头插入的方式，讲这个元素插入到对应的链表的表头
            d->ht[1].table[h] = de;///讲这个元素插入到数组对应的下标处
            d->ht[0].used--; ///ht[0]中的元素个数减一
            d->ht[1].used++; ///ht[1]中的元素个数加一
            de = nextde; ///指向下一个要处理的元素
        }
        d->ht[0].table[d->rehashidx] = NULL; ///将ht[0]中链表对应数组下标处的内容置空
        d->rehashidx++; ///更新rehashidx，将访问数组中下一个元素
    }
    
    ///检测我们是否已经已经完成了整个hash表的rehash操作
    if (d->ht[0].used == 0) { ///如果ht[0]中元素个数为0，表示已经进行完毕了
        zfree(d->ht[0].table); ///释放ht[0]的内存
        d->ht[0] = d->ht[1]; ///因为我们经常使用的ht[0],所以在完成rehash操作后，讲ht[0]指向ht[1]
        _dictReset(&d->ht[1]); ///重置ht[1]
        d->rehashidx = -1; ///将rehash标志设置为-1，表示未进行rehash操作
        return 0;
    }

    /* More to rehash... */
    return 1; ///表示有更多的节点需要进行rehash操作
}

///返回时间戳，单位为毫秒
long long timeInMilliseconds(void) {
    struct timeval tv;

    gettimeofday(&tv,NULL);
    return (((long long)tv.tv_sec)*1000)+(tv.tv_usec/1000);
}

///在规定的时间内，以10步为单位，对字典进行rehash操作，
int dictRehashMilliseconds(dict *d, int ms) {
    long long start = timeInMilliseconds(); ///记录开始时间
    int rehashes = 0;

    while(dictRehash(d,100)) {  ///rehash操作
        rehashes += 100; 
        if (timeInMilliseconds()-start > ms) break; ///到达指定的之间后，停止操作
    }
    return rehashes;
}

/* 此函数仅在没有哈希表绑定安全迭代器的情况下执行重新哈希的步骤。 当我们在重新哈希的中间有迭代器时，我们不能弄乱两个哈希表，
 * 否则某些元素可能会丢失或重复。 通过字典中的常见查找或更新操作调用此函数，以便在活跃使用哈希表时将其自动从H1迁移到H2。 
 */
static void _dictRehashStep(dict *d) {
    if (d->iterators == 0) dictRehash(d,1); ///如果没有迭代器，就进行1步rehash操作
}

///在字典中新增一个键值对
int dictAdd(dict *d, void *key, void *val)
{
    dictEntry *entry = dictAddRaw(d,key,NULL); ///在字典中创建一个键值并返回地址

    if (!entry) return DICT_ERR; ///如果创建失败，则直接返回
    dictSetVal(d, entry, val); ///将键值对写会到字典中
    return DICT_OK; ///返回操作成功
}

 /*
  *此函数添加键值对，不直接将这个键值对写入到对应的内存，而是直接返回申请的内存地址，这样用户就能够按照自己的意愿来写入数据。
  *此函数也直接暴露给要被调用的用户API，主要是为了将非指针存储在哈希值中，例如：
  *
  * entry = dictAddRaw（dict，mykey，NULL）;
  * 如果（entry！= NULL）dictSetSignedIntegerVal（entry，1000）;
  *
  * 返回值：
  * 如果键已经存在，则返回NULL，如果存在不为NULL，则使用现有条目填充“ * existing”。
  *如果添加了键，则哈希条目将返回以由调用方进行操作。
  */
dictEntry *dictAddRaw(dict *d, void *key, dictEntry **existing)
{
    long index;
    dictEntry *entry;
    dictht *ht;

    ///如果当前字典正在进行rehash操作，则进行1步rehash操作
    if (dictIsRehashing(d)) _dictRehashStep(d);

    ///根据key计算出该key在hash表中对应的数组下表，如果说key已经存在了，直接返回-1
    if ((index = _dictKeyIndex(d, key, dictHashKey(d,key), existing)) == -1)
        return NULL;

    ///分配内存并存储新的键值对。 假设在数据库系统中更有可能更频繁地访问最近添加的键值对，则将元素插入顶部。
    ht = dictIsRehashing(d) ? &d->ht[1] : &d->ht[0]; ///如果正在进行rehash操作，则将这个元素放到h[1]中，否则放到h[0]中
    entry = zmalloc(sizeof(*entry)); ///申请一个节点的内存空间
    entry->next = ht->table[index]; ///采用头插入的方式，将这个节点加入到链表的头部
    ht->table[index] = entry; ///讲这个节点放入到对应的数组下标的位置
    ht->used++; ///更新ht中的元素个数

    /* Set the hash entry fields. */
    dictSetKey(d, entry, key); ///将entry节点的值设置为key，并没有设置节点的值
    return entry; ///返回这个节点地址
}

///替换元素，如果key在dict中已经存在就将其val替换，如果不存在就添加到字典中。返回0表示存在，1表示不存在
int dictReplace(dict *d, void *key, void *val)
{
    dictEntry *entry, *existing, auxentry;
    
    ///如果key不存在，就尝试将这个键值对添加到字典中，病返回1
    entry = dictAddRaw(d,key,&existing);
    if (entry) {
        dictSetVal(d, entry, val);
        return 1;
    }
    
    ///如果这个key值在dict中已经存在了
    auxentry = *existing; ///拷贝当前的节点
    dictSetVal(d, existing, val); ///替换节点的值
    dictFreeVal(d, &auxentry); ///释放val的空间
    return 0; ///返回0
}

/*添加或查找：
 * dictAddOrFind（）只是dictAddRaw（）的一个版本，即使该键已经存在且无法添加，它也总是返回指定键的键值对（在这种情况下，将返回已存在键的键值对）。
 * 有关更多信息，请参见dictAddRaw（）。 
 */
dictEntry *dictAddOrFind(dict *d, void *key) {
    dictEntry *entry, *existing;
    entry = dictAddRaw(d,key,&existing); 
    return entry ? entry : existing;///如果说这个key对应的键值对已经存在，则返回存在的，如果不存在，也不能将它添加，因为entry只是一个地址
}

///搜索并删除一个元素。 这是dictDelete（）和dictUnlink（）的辅助函数
///nofree表示是否要释放key和value
static dictEntry *dictGenericDelete(dict *d, const void *key, int nofree) {
    uint64_t h, idx; 
    dictEntry *he, *prevHe;
    int table;

    if (d->ht[0].used == 0 && d->ht[1].used == 0) return NULL; ///如果是一个空字典，则直接返回
    if (dictIsRehashing(d)) _dictRehashStep(d); ///如果字典正在进行rehash操作，则让它进行1步rehash
    h = dictHashKey(d, key); ///计算key对应的hash值

    for (table = 0; table <= 1; table++) { 
        idx = h & d->ht[table].sizemask; //获取元素在数组中对应的下标
        he = d->ht[table].table[idx]; ///找到对应数组下标位置的元素（链表的头结点）
        prevHe = NULL;
        while(he) { ///遍历链表，直到为空
            if (key==he->key || dictCompareKeys(d, key, he->key)) { ///通过dictCompareKeys函数比较key是否相等，或者直接比较
                if (prevHe)
                    prevHe->next = he->next; ///如果该节点不是头结点，就直接让它的上一个节点指向它的下一个节点
                else
                    d->ht[table].table[idx] = he->next; ///如果是头结点，则直接将这个节点的下一个节点放如数组对应下标的位置
                if (!nofree) { ///如果说要释放key和val
                    dictFreeKey(d, he);  ///释放key
                    dictFreeVal(d, he);  ///释放val
                    zfree(he); ///释放这个节点
                }
                d->ht[table].used--; ///字典中的节点数减一
                return he; ///返回删除的节点
            }
            prevHe = he; ///给preHe赋值
            he = he->next; ///指向下一个节点
        }
        if (!dictIsRehashing(d)) break; ///如果说在hash表中没有找到，而且字典正在进行rehash操作，则直接退出循环
    }
    return NULL; ///没有找到，返回NULL
}

/// 删除一个元素，如果成功则返回DICT_OK，如果找不到该元素，则返回DICT_ERR。
int dictDelete(dict *ht, const void *key) {
    return dictGenericDelete(ht,key,0) ? DICT_OK : DICT_ERR;
}

/* 从表中删除一个元素，但实际上不释放键值对。 如果找到该元素（并从表中取消链接），则返回键值对，并且开发者应稍后调用
 * dictFreeUnlinkedEntry（）来释放它。 否则，如果找不到密钥，则返回NULL。
 * 当我们要从哈希表中删除某些内容，但想在实际删除条目之前使用其值时，此函数很有用。 没有此功能，该模式将需要两次查找：
 *
 * 条目= dictFind（...）;
 * //做一些输入
 * dictDelete（字典，条目）;
 *
 * 借助此功能，可以避免这种情况，而是使用：
 *
 * entry = dictUnlink（dictionary，entry）;
 * //做一些输入
 * dictFreeUnlinkedEntry（entry）; // <-不需要再次查找。
 */
dictEntry *dictUnlink(dict *ht, const void *key) {
    return dictGenericDelete(ht,key,1);
}

///您需要调用此函数才能在调用dictUnlink（）之后真正释放键值对。 使用'he'= NULL调用此函数是安全的。 
void dictFreeUnlinkedEntry(dict *d, dictEntry *he) {
    if (he == NULL) return;
    dictFreeKey(d, he); ///释放key
    dictFreeVal(d, he); ///释放val
    zfree(he); ///释放节点
}

///销毁这个字典
int _dictClear(dict *d, dictht *ht, void(callback)(void *)) {
    unsigned long i;

    ///释放所有的数据节点
    for (i = 0; i < ht->size && ht->used > 0; i++) { ///遍历整个链表
        dictEntry *he, *nextHe; 

        if (callback && (i & 65535) == 0) callback(d->privdata); ///调用callback函数对私有数据进行释放操作

        if ((he = ht->table[i]) == NULL) continue; ///如果hash表中数组当前下标处没有数据，则直接进入下一次循环
        while(he) { ///遍历整个链表，直至为空
            nextHe = he->next; ///获取下一个节点
            dictFreeKey(d, he); ///释放key
            dictFreeVal(d, he); ///释放value
            zfree(he); ///释放这个节点
            ht->used--; ///可用节点数减一
            he = nextHe; ///指向先一个接节点
        }
    }
    ///释放hash表
    zfree(ht->table);
    ///重置整个字典
    _dictReset(ht);
    return DICT_OK; ///因为为不会失败，直接返回操作成功
}

///清除和释放hash表
void dictRelease(dict *d)
{
    _dictClear(d,&d->ht[0],NULL); ///清除ht[0]
    _dictClear(d,&d->ht[1],NULL); ///清除ht[1]
    zfree(d); ///释放字典
}

///在字典中查询key是否存在，存在则返回地址
dictEntry *dictFind(dict *d, const void *key)
{
    dictEntry *he;
    uint64_t h, idx, table;

    if (dictSize(d) == 0) return NULL; ///如果字典为空，直接返回NUL
    if (dictIsRehashing(d)) _dictRehashStep(d); ///如果字典正在进行rehash操作，则直接将其转变为一步rehash
    h = dictHashKey(d, key); ///获取hash值
    for (table = 0; table <= 1; table++) { ///在hash表中进行查找
        idx = h & d->ht[table].sizemask;
        he = d->ht[table].table[idx];
        while(he) { //遍历链表
            if (key==he->key || dictCompareKeys(d, key, he->key))
                return he; ///找到元素，返回
            he = he->next;
        }
        if (!dictIsRehashing(d)) return NULL; ///如果没有找到元素，而没有进行rehash操作，直接返回NULL
    }
    return NULL;
}

///在字典中查询key对应的val
void *dictFetchValue(dict *d, const void *key) {
    dictEntry *he;

    he = dictFind(d,key); ///调用dictFind进行key查询
    return he ? dictGetVal(he) : NULL; ///如果key存在，再获取val，否则返回NULL
}

/* fingerprint是一个64位数字，代表给定时间的字典状态，它只是将dict属性固定在一起的结果。
 * 初始化不安全的迭代器后，我们将获得dictfingerprint，并在释放迭代器时再次检查fingerprint。
 * 如果两个fingerprint不同，则意味着迭代器的用户在迭代时对字典执行了禁止的操作。 
 */
///这个东西有类似于Java集合的modcount，整个机制有点类似于fail-fast机制
long long dictFingerprint(dict *d) {
    long long integers[6], hash = 0;
    int j;
    
    ///分别表示ht[0]和ht[1]两个表的地址，数组大小,元素个数
    integers[0] = (long) d->ht[0].table;
    integers[1] = d->ht[0].size;
    integers[2] = d->ht[0].used;
    integers[3] = (long) d->ht[1].table;
    integers[4] = d->ht[1].size;
    integers[5] = d->ht[1].used;

    /* We hash N integers by summing every successive integer with the integer
     * hashing of the previous sum. Basically:
     *
     * Result = hash(hash(hash(int1)+int2)+int3) ...
     *
     * This way the same set of integers in a different order will (likely) hash
     * to a different number. */
    for (j = 0; j < 6; j++) {
        hash += integers[j];
        /* For the hashing step we use Tomas Wang's 64 bit integer hash. */
        hash = (~hash) + (hash << 21); // hash = (hash << 21) - hash - 1;
        hash = hash ^ (hash >> 24);
        hash = (hash + (hash << 3)) + (hash << 8); // hash * 265
        hash = hash ^ (hash >> 14);
        hash = (hash + (hash << 2)) + (hash << 4); // hash * 21
        hash = hash ^ (hash >> 28);
        hash = hash + (hash << 31);
    }
    return hash;
}

///创建一个字典遍历的迭代器
dictIterator *dictGetIterator(dict *d)
{
    dictIterator *iter = zmalloc(sizeof(*iter)); ///申请迭代器的地址空间
    iter->d = d; ///迭代器指向的字典
    iter->table = 0; ///指向的hash表
    iter->index = -1; ///当前下标
    iter->safe = 0; ///是否安全,默认不安全
    iter->entry = NULL; ///
    iter->nextEntry = NULL; ///
    return iter;
}

///创建一个安全的迭代器
dictIterator *dictGetSafeIterator(dict *d) {
    dictIterator *i = dictGetIterator(d); ///调用不安全的迭代器创建

    i->safe = 1; ///safe标志设置为1即可
    return i;
}

///返回迭代器当前指向的节点
dictEntry *dictNext(dictIterator *iter)
{
    while (1) { ///
        if (iter->entry == NULL) { ///如果迭代器指向位置的节点元素为NULL
            dictht *ht = &iter->d->ht[iter->table]; ///获取迭代器的hash表
            if (iter->index == -1 && iter->table == 0) { ///如果迭代器指向数组下标为-1，并且指向的是ht[0]这个hash表
                if (iter->safe) ///如果是安全迭代器
                    iter->d->iterators++; ///让字典表的迭代器数据加一
                else
                    iter->fingerprint = dictFingerprint(iter->d); ///如果是不全的，就需要计算fingerprint 
            }
            iter->index++; //将迭代器指向数组中的下一个元素
            if (iter->index >= (long) ht->size) { ///如果迭代器的index没有越界
                if (dictIsRehashing(iter->d) && iter->table == 0) { ///如果字典正在进行rehash操作，并且ht[0]中没有元素
                    iter->table++; ///iter将指向ht[1]
                    iter->index = 0; ///index 归0
                    ht = &iter->d->ht[1]; 
                } else {
                    break;
                }
            }
            iter->entry = ht->table[iter->index]; ///得到哈希表中数组下一个元素的链表头节点
        } else {
            iter->entry = iter->nextEntry; ///获取当前节点的下一个节点
        }
        if (iter->entry) {
            ///我们需要在此处保存“下一个节点”，迭代器用户可能会删除我们返回的条目。 
            iter->nextEntry = iter->entry->next;
            return iter->entry; ///返回当前节点的位置
        }
    }
    return NULL;
}

///释放迭代器
void dictReleaseIterator(dictIterator *iter)
{
    if (!(iter->index == -1 && iter->table == 0)) {
        if (iter->safe)  ///如果是安全迭代器，只要讲字典的迭代器数量减一即可
            iter->d->iterators--;
        else ///如果是非安全的，就要判断fingerprint是否相等
            assert(iter->fingerprint == dictFingerprint(iter->d));
    }
    zfree(iter); ///释放迭代器
}

///从哈希表返回一个随机键值对。 有助于实现随机算法
dictEntry *dictGetRandomKey(dict *d)
{
    dictEntry *he, *orighe;
    unsigned long h;
    int listlen, listele;

    if (dictSize(d) == 0) return NULL; ///如果字典的大小为0，则直接返回NULL
    if (dictIsRehashing(d)) _dictRehashStep(d); ///如果此时正在进行rehash操作，则进行一步rehash操作
    if (dictIsRehashing(d)) {//如果正在进行rehash操作
        do {
            ///我们确定从0到rehashidx-1的索引中没有元素
            h = d->rehashidx + (random() % (d->ht[0].size +
                                            d->ht[1].size -
                                            d->rehashidx));
            he = (h >= d->ht[0].size) ? d->ht[1].table[h - d->ht[0].size] :
                                      d->ht[0].table[h];
        } while(he == NULL);///如果未获取到值，则重新进行一次操作
    } else { ///如果没有进行rehash操作，从ht[0]中查询一个随机的元素
        do {
            h = random() & d->ht[0].sizemask;
            he = d->ht[0].table[h];
        } while(he == NULL); ///如果为空，就需要重新进行一次操作
    }

    ///现在我们找到了一个非空的hash桶，这是一个链表，我们需要从列表中获取一个随机元素。明智的方法是对元素进行计数并选择一个随机索引。
    listlen = 0; ///用来记录链表的长度
    orighe = he;
    while(he) { ///遍历链表，获取链表长度
        he = he->next;
        listlen++;
    }
    listele = random() % listlen; ///在链表长度范围内获取一个随机数
    he = orighe; 
    while(listele--) he = he->next; ///遍历，获取对应随机数位置的节点
    return he; ///返回对应的节点
}

/* 此函数对字典进行采样，从随机位置返回一些键。
 *
 * 它不能保证返回'count'中指定的所有键，也不能保证返回非重复的元素。
 *
 * 返回的哈希表条目指针存储在指向dictEntry指针数组的'des'中。数组必须至少有“ count”个元素的空间，这是我们传递给函数的参数，用来告诉我们需要多少个随机元素。
 * 该函数返回存储到“ des”中的项目数，如果哈希表中包含少于“ count”个元素，或者在合理数量的步骤中找不到足够的元素，则该数目可能小于“ count”。
 *
 * 请注意，此功能仅在需要“采样”给定数量的连续元素以运行某种算法或生成统计信息时才适用，当您需要很好地分配返回的项目时，此功能不适用。
 * 但是，该函数在生成N个元素时比dictGetRandomKey（）快得多。 
 */
unsigned int dictGetSomeKeys(dict *d, dictEntry **des, unsigned int count) {
    unsigned long j; /*内部哈希表ID，0或1。*/
    unsigned long tables; ///用来记录需要访问的hash表的数量，1或者2
    unsigned long stored = 0, maxsizemask;
    unsigned long maxsteps;

    if (dictSize(d) < count) count = dictSize(d); ///如果字典中的元素个数小于count，则将count的值设置为字典中元素的个数
    maxsteps = count*10;

    /* Try to do a rehashing work proportional to 'count'. */
    for (j = 0; j < count; j++) {
        if (dictIsRehashing(d)) ///如果正在进行rehash操作
            _dictRehashStep(d); ///进行一步rehash操作
        else ///如果没有, 就直接跳出循环
            break; 
    }

    tables = dictIsRehashing(d) ? 2 : 1; ///如果正在rehash操作，就需要访问两张表，否则只需要访问一张表
    maxsizemask = d->ht[0].sizemask; ///设置最大偏移量
    if (tables > 1 && maxsizemask < d->ht[1].sizemask) 
        maxsizemask = d->ht[1].sizemask; ///将偏移量设置为ht[0]、ht[1]中hash数组较大的那个数组的长度

    /* Pick a random point inside the larger table. */
    unsigned long i = random() & maxsizemask; ///找到一个在最大偏移量中的一个随机数
    unsigned long emptylen = 0; 
    while(stored < count && maxsteps--) {
        for (j = 0; j < tables; j++) { ///在表中进行查找，可能是一张表，也有可能是两张表
            
			/// dict.c重新哈希的不变性：在重新哈希期间，直到在ht[0]中已访问的索引为止，没有填充的hash桶，因此对于0到idx-1之间的索引，我们可以跳过ht[0]。
            if (tables == 2 && j == 0 && i < (unsigned long) d->rehashidx) { //
               
			    ///而且，如果当前我们不在第二个表的范围内，则两个表中都没有元素到当前的重新哈希索引为止，因此，如果可能，我们跳转。（从大表到小表时会发生这种情况）。
                if (i >= d->ht[1].size)
                    i = d->rehashidx;
                else
                    continue;
            }
            if (i >= d->ht[j].size) continue; ///如果i大于了当前hash的大小，则已经越界了
            dictEntry *he = d->ht[j].table[i]; ///查询的hash桶
			
            if (he == NULL) { ///如果hash桶为空
                emptylen++; ///将记录空hash桶的数量加一
                if (emptylen >= 5 && emptylen > count) { ///如果空桶的数量大于等于5并且大于了count的数量
                    i = random() & maxsizemask; ///就从新设置最大偏移量的值
                    emptylen = 0; ///将记录空桶的数值置为0
                }
            } else { ///对应的hash桶不为空
                emptylen = 0;  ///将记录空桶的数值置为0
                while (he) { ///遍历链表
                    *des = he; ///存储已找到的节点
                    des++; ///指向des的下一个位置（des是一个节点数组，用来存放满足条件的节点）
                    he = he->next; ///指向下一个元素
                    stored++; ///用来保存获取的随机key的数量的值加一
                    if (stored == count) return stored; //如果需要的数量已经达到了我们的要求，就直接返回即可
                }
            }
        }
        i = (i+1) & maxsizemask; ///查询下一个hash桶
    }
    return stored; ///返回查询的数量
}

/* 就像API的POV中的dictGetRandomKey（）一样，但是将做更多的工作来确保更好地分配返回的元素。
 *
 * 此函数改善了分布，因为dictGetRandomKey（）问题是它选择一个随机存储桶，然后从存储桶中的链中选择一个随机元素。 
 * 但是，处于不同链长的元素将有不同的概率被查询。 使用此功能，我们要做的是考虑表的“线性”范围，该范围可能由N个存储桶组成，不同长度的链条依次出现。 然后，我们查询该范围内的随机元素。
 * 通过这种方式，我们解决了不同链条长度的问题。   
 */
#define GETFAIR_NUM_ENTRIES 15
dictEntry *dictGetFairRandomKey(dict *d) { ///公平的获取随机key
    dictEntry *entries[GETFAIR_NUM_ENTRIES]; ///定义一个数组，用来保存key
    unsigned int count = dictGetSomeKeys(d,entries,GETFAIR_NUM_ENTRIES); ///从字典中获取15个key，但是返回结果不一定是15个
    /* Note that dictGetSomeKeys() may return zero elements in an unlucky
     * run() even if there are actually elements inside the hash table. So
     * when we get zero, we call the true dictGetRandomKey() that will always
     * yeld the element if the hash table has at least one. */
    if (count == 0) return dictGetRandomKey(d); ///如果返回结果为0，则直接查询一个key
    unsigned int idx = rand() % count; ///否则获取一个随机下标
    return entries[idx]; ///返回数组中对应下标的值
}

///反转bits的方法. 算法来源于: http://graphics.stanford.edu/~seander/bithacks.html#ReverseParallel
static unsigned long rev(unsigned long v) {
    unsigned long s = CHAR_BIT * sizeof(v); //bit位数的大小必须是2的指数
    unsigned long mask = ~0UL; ///所有位数全部为1
    while ((s >>= 1) > 0) {
        mask ^= (mask << s);
        v = ((v >> s) & mask) | ((v << s) & ~mask);
    }
    return v;
}

/*该函数用做字典的遍历
 *
 *迭代的工作方式如下：
 *
 * 1）最初，您使用0作为游标（v）调用函数。
 * 2）该函数执行迭代的第一步，并返回在下一次调用中必须使用的新游标值。
 * 3）返回的游标为0时，迭代完成。
 *
 * 该函数保证字典中存在的所有元素在迭代的开始和结束之间被返回。 但是，某些元素有可能多次返回。
 * 每当返回一个元素时，调用fu函数， 以“ privdata”作为第一个参数，并以字典节点“de”作为第二个参数。
 *
 * 它的运行思路：
 * 迭代算法由Pieter Noordhuis设计。 主要思想是从高阶位开始增加光标。 即，不是正常地增加光标，而是反转光标的位，然后增加光标，最后再次反转位。
 * 此策略是必需的，因为在迭代调用之间可能会调整哈希表的大小。
 * dict.c哈希表的大小始终是2的幂，并且它们使用链接，因此，给定表中元素的位置是通过计算Hash（key）与SIZE-1（其中SIZE-1）之间按位与给出的 始终是掩码，
 * 等同于对键的哈希值和SIZE进行其余的划分。
 * 例如，如果当前哈希表的大小为16，则掩码为（二进制）1111。键在哈希表中的位置将始终为哈希输出的最后四位，依此类推。
 *
 * 如果hash表大小发生变化，会发生什么？
 * 如果散列表增加，则元素可以以旧存储桶的倍数到达任意位置：例如，假设我们已经使用4位游标1100进行了迭代（掩码为1111，因为散列表的大小= 16）。
 * 如果将哈希表的大小调整为64个元素，那么新掩码将为111111。通过在1100中替换0或1所获得的新存储桶只能由我们在扫描存储桶1100时已经访问过的键作为目标。在较小的哈希表中。
 * 由于计数器是反向的，因此首先迭代较高的位，如果表大小变大，则无需重新启动游标。它会使用没有结尾的1100游标继续进行迭代，并且也不会探索最后4位的任何其他组合。
 * 同样，当表的大小随着时间的推移而缩小时，例如从16减小到8，如果已经完全探究了低三位的组合（大小8的掩码为111），则不会再次访问它，
 * 因为我们确信例如，我们尝试了0111和1111（较高位的所有变体），因此我们无需再次对其进行测试。
 *
 * 稍等！！！在重新哈希过程中，您有两张hash表！
 * 是的，这是对的，但是我们总是首先迭代较小的表，然后再测试当前游标对较大表的所有扩展。 例如，如果当前光标为101，并且我们还有一个大小为16的较大表，
 * 我们还将在较大表中测试（0）101和（1）101。 这样可以将问题减少到只有一个表，而较大的表（如果存在）只是较小的表的扩展。
 *
 *限制
 * 此迭代器是完全无状态的，这是一个巨大的优势，其中不包括使用额外的内存。这种设计的缺点是：
 * 1）我们可能不止一次返回元素。 但是，这通常在应用程序级别很容易处理。
 * 2）迭代器必须在每次调用时返回多个元素，因为它需要始终返回在给定存储桶中链接的所有键以及所有扩展，因此我们确保在散列过程中不会丢失键移动。
 * 3）刚开始时，反向光标有些难以理解，但是应该可以使用此注释。
 */
unsigned long dictScan(dict *d,
                       unsigned long v,
                       dictScanFunction *fn,
                       dictScanBucketFunction* bucketfn,
                       void *privdata)
{
    dictht *t0, *t1;
    const dictEntry *de, *next;
    unsigned long m0, m1;

    if (dictSize(d) == 0) return 0; ///如果字典为空，则直接返回

    ///拥有安全的迭代器意味着无法进行任何重新哈希处理，请参见_dictRehashStep。 如果扫描回调试图执行dictFind或类似操作，则需要此方法。
    d->iterators++;

    if (!dictIsRehashing(d)) { ///如果没有进行rehash操作，则所有的数据都存在一个hash表中(ht[0]中）
        t0 = &(d->ht[0]);  ///t0指向ht[0]
        m0 = t0->sizemask; ///偏移量设置为t0的数组大小

        /* Emit entries at cursor */
        if (bucketfn) bucketfn(privdata, &t0->table[v & m0]); ///如果有定义bucketfn这个函数，则调用这个函数，查询对应的hash桶
        de = t0->table[v & m0]; ///指向一个hash桶
        while (de) { ///遍历这个hash桶
            next = de->next; 
            fn(privdata, de); ///调用fn函数， privdata作为他的第一个参数，de第二个参数
            de = next;
        }

        ///设置未屏蔽的位，以便使反向光标递增对屏蔽的位进行操作
        v |= ~m0;

       ///增加这个反向光标
        v = rev(v);
        v++;
        v = rev(v);

    } else { ///如果正在进行rehash操作，就需要查询两个hash表
        t0 = &d->ht[0];
        t1 = &d->ht[1];

        ///必须要确认t0所指向的表是小表，t1所指向的表是大表
        if (t0->size > t1->size) {
            t0 = &d->ht[1];
            t1 = &d->ht[0];
        }

        m0 = t0->sizemask; ///设置偏移量
        m1 = t1->sizemask;

        ///然后进行上面一样的操作
        if (bucketfn) bucketfn(privdata, &t0->table[v & m0]);
        de = t0->table[v & m0];
        while (de) {
            next = de->next;
            fn(privdata, de);
            de = next;
        }

        ///迭代较大表中的索引，这些索引是较小表中光标指向的索引的扩展
        do {
            /* Emit entries at cursor */
            if (bucketfn) bucketfn(privdata, &t1->table[v & m1]);
            de = t1->table[v & m1]; ///获取哈希桶
            while (de) { ///遍历整个链表
                next = de->next;
                fn(privdata, de); ///调用fu函数
                de = next;
            }

            /* Increment the reverse cursor not covered by the smaller mask.*/
            v |= ~m1;
            v = rev(v);
            v++;
            v = rev(v);

            /* Continue while bits covered by mask difference is non-zero */
        } while (v & (m0 ^ m1));
    }

    ///减少安全迭代器的数量
    d->iterators--;
	///返回查询的结果
    return v;
}

/* ------------------------- 私有方法 ------------------------------ */

///如果需要扩展字典的话就进行扩展
static int _dictExpandIfNeeded(dict *d)
{
    ///如果正在进行rehash操作，就直接返回
    if (dictIsRehashing(d)) return DICT_OK;

    ///如果hash表为空的时候，就将其扩展为默认的大小
    if (d->ht[0].size == 0) return dictExpand(d, DICT_HT_INITIAL_SIZE);

    ///如果达到1：1的比例，并且允许我们调整哈希表的大小（全局设置），或者应避免使用该哈希表，但是元素/存储桶之间的比率超过“安全”阈值，则我们将大小调整为原来的2倍。
    if (d->ht[0].used >= d->ht[0].size &&  ///达到1：1的比例
        (dict_can_resize || ///允许调整哈希表的大小
         d->ht[0].used/d->ht[0].size > dict_force_resize_ratio)) ///元素/存储桶之间的比率超过“安全”阈值
    {
        return dictExpand(d, d->ht[0].used*2); ///进行字典扩容操作
    }
    return DICT_OK; ///返回操作成功
}

///hash表的数组大小一定要是2的指数
static unsigned long _dictNextPower(unsigned long size)
{
    unsigned long i = DICT_HT_INITIAL_SIZE; ///默认的初始化大小 ，大小为4
    if (size >= LONG_MAX) return LONG_MAX + 1LU; ///Size最大只能为LONG_MAX
    while(1) {
        if (i >= size)
            return i;
        i *= 2; ///按照2倍的倍率扩大
    }
}

 /* 返回可用插槽的索引，该插槽可以用给定“键”的哈希条目填充。
  * 如果键已经存在，则返回-1，并且可以填写可选的输出参数。
  *
  * 请注意，如果我们正在重新哈希表，则总是在第二个（新）哈希表的上下文中返回索引。 
  */
///返回键为key在哈此表中的下标
static long _dictKeyIndex(dict *d, const void *key, uint64_t hash, dictEntry **existing)
{
    unsigned long idx, table;
    dictEntry *he;
    if (existing) *existing = NULL;

    /* Expand the hash table if needed */
    if (_dictExpandIfNeeded(d) == DICT_ERR) ///插入前如果需要进行扩容操作，则直接返回-1
        return -1;
    for (table = 0; table <= 1; table++) { //遍历ht[0]、ht[1]
        idx = hash & d->ht[table].sizemask; ///获取key所在的数组下标
        he = d->ht[table].table[idx]; ///判断对应的hash桶中是否包含有该key
        while(he) {
            if (key==he->key || dictCompareKeys(d, key, he->key)) { ///如果包含有该key
                if (existing) *existing = he; ///将这个key赋值给existing，并返回-1
                return -1;
            }
            he = he->next; 
        }
        if (!dictIsRehashing(d)) break; ///如果没有进行rehash操作，表示不用遍历ht[1],直接跳出循环
    }
    return idx; ///返回找到的数组下标
}

///清空字典中的两个hash表，并重置字典
void dictEmpty(dict *d, void(callback)(void*)) {
    _dictClear(d,&d->ht[0],callback); ///清空ht[0]
    _dictClear(d,&d->ht[1],callback); ///清空ht[1]
    d->rehashidx = -1; ///重置rehashidx
    d->iterators = 0; ///重置iterator的数量
}

///开启字典扩容的参数
void dictEnableResize(void) {
    dict_can_resize = 1;
}

///关闭字典扩容的参数
void dictDisableResize(void) {
    dict_can_resize = 0;
}

///找到key对应hash值
uint64_t dictGetHash(dict *d, const void *key) {
    return dictHashKey(d, key);
}

/* 通过使用指针和预先计算的哈希查找dictEntry引用。 oldkey是无效指针，不应访问。 哈希值应使用dictGetHash提供。 不执行字符串/键比较。
 * 返回值是对dictEntry的引用（如果找到），否则为NULL。 
 */
dictEntry **dictFindEntryRefByPtrAndHash(dict *d, const void *oldptr, uint64_t hash) {
    dictEntry *he, **heref;
    unsigned long idx, table;

    if (dictSize(d) == 0) return NULL; ///字典为空，则直接返回NULL
    for (table = 0; table <= 1; table++) { ///遍历一张或者两张表
        idx = hash & d->ht[table].sizemask; ///获取该key在hash表中的数组下标
        heref = &d->ht[table].table[idx]; ///找到对应的hash桶
        he = *heref; 
        while(he) { ///遍历整个链表
            if (oldptr==he->key) ///如果oldptr和该节点的key相同、
                return heref; ///返回该节点
            heref = &he->next;
            he = *heref;
        }
        if (!dictIsRehashing(d)) return NULL; ///如果没有进行rehash操作，表示只用遍历一张表，直接返回NULL
    }
    return NULL;
}

/* ------------------------------- Debugging ---------------------------------*/

#define DICT_STATS_VECTLEN 50
size_t _dictGetStatsHt(char *buf, size_t bufsize, dictht *ht, int tableid) {
    unsigned long i, slots = 0, chainlen, maxchainlen = 0;
    unsigned long totchainlen = 0;
    unsigned long clvector[DICT_STATS_VECTLEN];
    size_t l = 0;

    if (ht->used == 0) {
        return snprintf(buf,bufsize,
            "No stats available for empty dictionaries\n");
    }

    /* Compute stats. */
    for (i = 0; i < DICT_STATS_VECTLEN; i++) clvector[i] = 0;
    for (i = 0; i < ht->size; i++) {
        dictEntry *he;

        if (ht->table[i] == NULL) {
            clvector[0]++;
            continue;
        }
        slots++;
        /* For each hash entry on this slot... */
        chainlen = 0;
        he = ht->table[i];
        while(he) {
            chainlen++;
            he = he->next;
        }
        clvector[(chainlen < DICT_STATS_VECTLEN) ? chainlen : (DICT_STATS_VECTLEN-1)]++;
        if (chainlen > maxchainlen) maxchainlen = chainlen;
        totchainlen += chainlen;
    }

    /* Generate human readable stats. */
    l += snprintf(buf+l,bufsize-l,
        "Hash table %d stats (%s):\n"
        " table size: %ld\n"
        " number of elements: %ld\n"
        " different slots: %ld\n"
        " max chain length: %ld\n"
        " avg chain length (counted): %.02f\n"
        " avg chain length (computed): %.02f\n"
        " Chain length distribution:\n",
        tableid, (tableid == 0) ? "main hash table" : "rehashing target",
        ht->size, ht->used, slots, maxchainlen,
        (float)totchainlen/slots, (float)ht->used/slots);

    for (i = 0; i < DICT_STATS_VECTLEN-1; i++) {
        if (clvector[i] == 0) continue;
        if (l >= bufsize) break;
        l += snprintf(buf+l,bufsize-l,
            "   %s%ld: %ld (%.02f%%)\n",
            (i == DICT_STATS_VECTLEN-1)?">= ":"",
            i, clvector[i], ((float)clvector[i]/ht->size)*100);
    }

    /* Unlike snprintf(), teturn the number of characters actually written. */
    if (bufsize) buf[bufsize-1] = '\0';
    return strlen(buf);
}

void dictGetStats(char *buf, size_t bufsize, dict *d) {
    size_t l;
    char *orig_buf = buf;
    size_t orig_bufsize = bufsize;

    l = _dictGetStatsHt(buf,bufsize,&d->ht[0],0);
    buf += l;
    bufsize -= l;
    if (dictIsRehashing(d) && bufsize > 0) {
        _dictGetStatsHt(buf,bufsize,&d->ht[1],1);
    }
    /* Make sure there is a NULL term at the end. */
    if (orig_bufsize) orig_buf[orig_bufsize-1] = '\0';
}

/* ------------------------------- Benchmark ---------------------------------*/

#ifdef DICT_BENCHMARK_MAIN

#include "sds.h"

uint64_t hashCallback(const void *key) {
    return dictGenHashFunction((unsigned char*)key, sdslen((char*)key));
}

int compareCallback(void *privdata, const void *key1, const void *key2) {
    int l1,l2;
    DICT_NOTUSED(privdata);

    l1 = sdslen((sds)key1);
    l2 = sdslen((sds)key2);
    if (l1 != l2) return 0;
    return memcmp(key1, key2, l1) == 0;
}

void freeCallback(void *privdata, void *val) {
    DICT_NOTUSED(privdata);

    sdsfree(val);
}

dictType BenchmarkDictType = {
    hashCallback,
    NULL,
    NULL,
    compareCallback,
    freeCallback,
    NULL
};

#define start_benchmark() start = timeInMilliseconds()
#define end_benchmark(msg) do { \
    elapsed = timeInMilliseconds()-start; \
    printf(msg ": %ld items in %lld ms\n", count, elapsed); \
} while(0);

/* dict-benchmark [count] */
int main(int argc, char **argv) {
    long j;
    long long start, elapsed;
    dict *dict = dictCreate(&BenchmarkDictType,NULL);
    long count = 0;

    if (argc == 2) {
        count = strtol(argv[1],NULL,10);
    } else {
        count = 5000000;
    }

    start_benchmark();
    for (j = 0; j < count; j++) {
        int retval = dictAdd(dict,sdsfromlonglong(j),(void*)j);
        assert(retval == DICT_OK);
    }
    end_benchmark("Inserting");
    assert((long)dictSize(dict) == count);

    /* Wait for rehashing. */
    while (dictIsRehashing(dict)) {
        dictRehashMilliseconds(dict,100);
    }

    start_benchmark();
    for (j = 0; j < count; j++) {
        sds key = sdsfromlonglong(j);
        dictEntry *de = dictFind(dict,key);
        assert(de != NULL);
        sdsfree(key);
    }
    end_benchmark("Linear access of existing elements");

    start_benchmark();
    for (j = 0; j < count; j++) {
        sds key = sdsfromlonglong(j);
        dictEntry *de = dictFind(dict,key);
        assert(de != NULL);
        sdsfree(key);
    }
    end_benchmark("Linear access of existing elements (2nd round)");

    start_benchmark();
    for (j = 0; j < count; j++) {
        sds key = sdsfromlonglong(rand() % count);
        dictEntry *de = dictFind(dict,key);
        assert(de != NULL);
        sdsfree(key);
    }
    end_benchmark("Random access of existing elements");

    start_benchmark();
    for (j = 0; j < count; j++) {
        sds key = sdsfromlonglong(rand() % count);
        key[0] = 'X';
        dictEntry *de = dictFind(dict,key);
        assert(de == NULL);
        sdsfree(key);
    }
    end_benchmark("Accessing missing");

    start_benchmark();
    for (j = 0; j < count; j++) {
        sds key = sdsfromlonglong(j);
        int retval = dictDelete(dict,key);
        assert(retval == DICT_OK);
        key[0] += 17; /* Change first number to letter. */
        retval = dictAdd(dict,key,(void*)j);
        assert(retval == DICT_OK);
    }
    end_benchmark("Removing and adding");
}
#endif
