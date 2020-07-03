#include <string.h> 
#include "quicklist.h"
#include "zmalloc.h"
#include "ziplist.h"
#include "util.h"
#include "lzf.h"

#if defined(REDIS_TEST) || defined(REDIS_TEST_VERBOSE)
#include <stdio.h> /* for printf (debug printing), snprintf (genstr) */
#endif

#ifndef REDIS_STATIC
#define REDIS_STATIC static
#endif

/* Optimization levels for size-based filling */
static const size_t optimization_level[] = {4096, 8192, 16384, 32768, 65536};

/* Maximum size in bytes of any multi-element ziplist.
 * Larger values will live in their own isolated ziplists. */
///每个节点的中压缩表的最大大小，
#define SIZE_SAFETY_LIMIT 8192

/* Minimum ziplist size in bytes for attempting compression. */
///压缩表能够进行压缩的最小长度
#define MIN_COMPRESS_BYTES 48

/* Minimum size reduction in bytes to store compressed quicklistNode data.
 * This also prevents us from storing compression if the compression
 * resulted in a larger size than the original data. */
///存储压缩的quicklistNode数据的最小大小（以字节为单位）。 如果压缩后的大小大于原始数据的大小，将阻止压缩操作。 
#define MIN_COMPRESS_IMPROVE 8

/* If not verbose testing, remove all debug printing. */
#ifndef REDIS_TEST_VERBOSE
#define D(...)
#else
#define D(...)                                                                 \
    do {                                                                       \
        printf("%s:%s:%d:\t", __FILE__, __FUNCTION__, __LINE__);               \
        printf(__VA_ARGS__);                                                   \
        printf("\n");                                                          \
    } while (0);
#endif

/* Bookmarks forward declarations */
#define QL_MAX_BM ((1 << QL_BM_BITS)-1)
quicklistBookmark *_quicklistBookmarkFindByName(quicklist *ql, const char *name);
quicklistBookmark *_quicklistBookmarkFindByNode(quicklist *ql, quicklistNode *node);
void _quicklistBookmarkDelete(quicklist *ql, quicklistBookmark *bm);

/* Simple way to give quicklistEntry structs default values with one call. */
///初始化快表节点中的压缩表信息
#define initEntry(e)                                                           \
    do {                                                                       \
        (e)->zi = (e)->value = NULL; ///初始化压缩表及其值                        \
        (e)->longval = -123456789;   ///初始化压缩表的大小（字节数）                \
        (e)->quicklist = NULL;       ///初始化压缩表属于哪个快表                   \
        (e)->node = NULL;            ///初始化压缩表处与快表中那个节点              \
        (e)->offset = 123456789;     ///初始化当前压缩表的的偏移量                 \
        (e)->sz = 0;                 ///初始化当前压缩表的节点数                   \
    } while (0)

#if __GNUC__ >= 3
#define likely(x) __builtin_expect(!!(x), 1) ///x为真的可能性大
#define unlikely(x) __builtin_expect(!!(x), 0) ///x为假的可能性大
#else
#define likely(x) (x)
#define unlikely(x) (x)
#endif

/// 新建一个快表，同时可以使用quicklistRelease()释放快表
quicklist *quicklistCreate(void) {
    struct quicklist *quicklist; ///定义快表指针

    quicklist = zmalloc(sizeof(*quicklist)); ///申请内存地址
    quicklist->head = quicklist->tail = NULL; ///初始化快表的头节点和尾节点
    quicklist->len = 0; ///初始化快表中的节点数
    quicklist->count = 0; ///初始化快表中压缩表的节点数
    quicklist->compress = 0; ///是否进行压缩操作，默认是不进行压缩
    quicklist->fill = -2; ///设置默认值，每个ziplist的字节数最大为8kb
    quicklist->bookmark_count = 0; ///快表中bookmark的数量
    return quicklist;
}

#define COMPRESS_MAX ((1 << QL_COMP_BITS)-1)///QL_COMP_BITS = 16，最大压缩的最大长度为2^16 -1
void quicklistSetCompressDepth(quicklist *quicklist, int compress) { ///设置压缩的程度
    if (compress > COMPRESS_MAX) { ///如果压缩程度大于最大压缩长度
        compress = COMPRESS_MAX; ///让它等于最大的压缩程度
    } else if (compress < 0) { ///如果小于0
        compress = 0; ///让它等于0， 不进行压缩
    }
    quicklist->compress = compress; ///设置快表的压缩程度
} 

#define FILL_MAX ((1 << (QL_FILL_BITS-1))-1) ///每个压缩表最多有2^15 -1个节点
void quicklistSetFill(quicklist *quicklist, int fill) { ///设置快表中压缩表节点的大小
    if (fill > FILL_MAX) { ///如果大于最大的值
        fill = FILL_MAX; ///让它等于最大值
    } else if (fill < -5) { ///ziplist字节数最大可以为64kb, 在redis.conf文件中有设置
        fill = -5;
    }
    quicklist->fill = fill; //
}

///设置快表fill和depth两个参数
void quicklistSetOptions(quicklist *quicklist, int fill, int depth) {
    quicklistSetFill(quicklist, fill);
    quicklistSetCompressDepth(quicklist, depth);
}

///创建一个新的快表，快表中的元素的值为默认
quicklist *quicklistNew(int fill, int compress) {
    quicklist *quicklist = quicklistCreate();
    quicklistSetOptions(quicklist, fill, compress);
    return quicklist;
}

///创建新的快表节点，并初始化节点中的值
REDIS_STATIC quicklistNode *quicklistCreateNode(void) {
    quicklistNode *node; ///声明指针
    node = zmalloc(sizeof(*node)); ///为这个指针分配内存
    node->zl = NULL; ///设置这个节点的ziplist
    node->count = 0; ///设置节点中的压缩表大小
    node->sz = 0; ///设置节点中压缩表的节点数量
    node->next = node->prev = NULL; ///设置当前节点的前驱节点和后继节点
    node->encoding = QUICKLIST_NODE_ENCODING_RAW; ///设置是否进行压缩，默认为不压缩
    node->container = QUICKLIST_NODE_CONTAINER_ZIPLIST; ///设置存书数据的数据结构，默认是压缩表
    node->recompress = 0; ///设置是否进行压缩，0表示不进行压缩
    return node;
}

/* Return cached quicklist count */
///返回快表中压缩表中的节点个数
unsigned long quicklistCount(const quicklist *ql) { return ql->count; }

///释放快表
void quicklistRelease(quicklist *quicklist) {
    unsigned long len;
    quicklistNode *current, *next;

    current = quicklist->head; ///从头节点开始进行释放
    len = quicklist->len; ///获取快表的节点数量
    while (len--) { ///进行循环释放操作
        next = current->next; ///获取下一个节点

        zfree(current->zl); ///释放当前节点的压缩表
        quicklist->count -= current->count; ///在快表的压缩表记录项中修改其值

        zfree(current); ///释放当前的快表节点

        quicklist->len--; ///修改快表的节点数量
        current = next; ///修改指针，指向下一个节点
    }
    quicklistBookmarksClear(quicklist); ///释放Bookmark
    zfree(quicklist); ///释放快表
}

/// 对节点进行压缩操作，返回1表示压缩成功， 0表示压缩失败或者压缩表太小，不能进行压缩操作
REDIS_STATIC int __quicklistCompressNode(quicklistNode *node) {
#ifdef REDIS_TEST
    node->attempted_compress = 1; ///将节点的压缩标志设置为可压缩
#endif
    /* Don't bother compressing small values */
    if (node->sz < MIN_COMPRESS_BYTES) ///如果压缩表的大小小于最小值（48字节），直接返回0
        return 0;

    quicklistLZF *lzf = zmalloc(sizeof(*lzf) + node->sz); ///分配压缩后需要的空间

    ///如果压缩失败或者压缩空间太小了，直接返回0
    if (((lzf->sz = lzf_compress(node->zl, node->sz, lzf->compressed,
                                 node->sz)) == 0) ||
        lzf->sz + MIN_COMPRESS_IMPROVE >= node->sz) {
        /* lzf_compress aborts/rejects compression if value not compressable. */
        zfree(lzf); ///不能进行压缩操作，释放前面申请的空间
        return 0;
    }
    lzf = zrealloc(lzf, sizeof(*lzf) + lzf->sz); ///压缩成功并分配压缩成功大小的空间
    zfree(node->zl); ///释放原来的压缩表的空间
    node->zl = (unsigned char *)lzf; ///将新的空间赋值给当前的快表节点
    node->encoding = QUICKLIST_NODE_ENCODING_LZF; ///把节点的编码格式设置为LZF类型
    node->recompress = 0;///设置压缩标志。0表示不用进行压缩操作
    return 1; ///返回操作成功
}

/* Compress only uncompressed nodes. */
///只压缩没有压缩过的节点
#define quicklistCompressNode(_node)                                           \
    do {                                                                       \
        ///如果节点的编码类型为QUICKLIST_NODE_ENCODING_RAW，表示没有压缩过           \
        if ((_node) && (_node)->encoding == QUICKLIST_NODE_ENCODING_RAW) {     \
            __quicklistCompressNode((_node));                                  \
        }                                                                      \
    } while (0)

///解压压缩过的节点，并设置它的压缩编码，操作成功返回1，操作失败返回0
REDIS_STATIC int __quicklistDecompressNode(quicklistNode *node) {
#ifdef REDIS_TEST
    node->attempted_compress = 0;
#endif

    void *decompressed = zmalloc(node->sz);  ///申请解压所需要的空间
    quicklistLZF *lzf = (quicklistLZF *)node->zl; ///获取压缩的节点
    ///进行解压操作，如果返回0，表示解压失败
    if (lzf_decompress(lzf->compressed, lzf->sz, decompressed, node->sz) == 0) {
        /* Someone requested decompress, but we can't decompress.  Not good. */
        zfree(decompressed); ///释放申请的解压缩空间
        return 0; ///返回操作失败
    }
    zfree(lzf); ///操作成功，释放原来的压缩的空间
    node->zl = decompressed;  ///将解压缩空间赋值给当前快表节点
    node->encoding = QUICKLIST_NODE_ENCODING_RAW; ///修改快表节点的压缩类型为QUICKLIST_NODE_ENCODING_RAW
    return 1; ///返回操作成功
}

/* Decompress only compressed nodes. */
///对没有解压的节点进行解压缩操作
#define quicklistDecompressNode(_node)                                         \
    do {                                                                       \
        if ((_node) && (_node)->encoding == QUICKLIST_NODE_ENCODING_LZF) {     \
            __quicklistDecompressNode((_node));                                \
        }                                                                      \
    } while (0)

/* Force node to not be immediately re-compresable */
///标记已经被压缩的节点，等待被再一次压缩
#define quicklistDecompressNodeForUse(_node)                                   \
    do {                                                                       \
        if ((_node) && (_node)->encoding == QUICKLIST_NODE_ENCODING_LZF) {     \
            __quicklistDecompressNode((_node));                                \
            (_node)->recompress = 1;                                           \
        }                                                                      \
    } while (0)

///从快表中获取压缩过的压缩表的大小，并且将这个压缩过的数据保存到data中
size_t quicklistGetLzf(const quicklistNode *node, void **data) {
    quicklistLZF *lzf = (quicklistLZF *)node->zl; ///获取压缩后的压缩表
    *data = lzf->compressed; ///将数据地址保存到data中
    return lzf->sz; ///返回压缩后压缩表的大小
}

///返回快表节点是否可压缩，1表示可压缩，0表示不可压缩
#define quicklistAllowsCompression(_ql) ((_ql)->compress != 0)

/* Force 'quicklist' to meet compression guidelines set by compress depth.
 * The only way to guarantee interior nodes get compressed is to iterate
 * to our "interior" compress depth then compress the next node we find.
 * If compress depth is larger than the entire list, we return immediately. */
/// 在快表中查询node，如果node不在压缩程度的范围内，就进行压缩操作
REDIS_STATIC void __quicklistCompress(const quicklist *quicklist,
                                      quicklistNode *node) {
    /* If length is less than our compress depth (from both sides),
     * we can't compress anything. */
    ///如果快表不能被压缩或者节点的压缩表长度小于压缩的深度，就无法进行压缩操作
    if (!quicklistAllowsCompression(quicklist) ||
        quicklist->len < (unsigned int)(quicklist->compress * 2))
        return;

#if 0
    /* Optimized cases for small depth counts */
    if (quicklist->compress == 1) { ///如果压缩标志为1，表示可进行压缩操作
        quicklistNode *h = quicklist->head, *t = quicklist->tail; ///获取快表的头节点和尾节点
        quicklistDecompressNode(h);///如果需要解压，就进行解压缩操作
        quicklistDecompressNode(t);
        if (h != node && t != node) ///如果头节点和尾节点都不是我们要找的节点
            quicklistCompressNode(node); ///就对该node进行压缩操作
        return;
    } else if (quicklist->compress == 2) {///如果压缩标志为2，两端的两个节点
        quicklistNode *h = quicklist->head, *hn = h->next, *hnn = hn->next;
        quicklistNode *t = quicklist->tail, *tp = t->prev, *tpp = tp->prev;
        quicklistDecompressNode(h);///如果需要解压，就进行解压缩操作
        quicklistDecompressNode(hn);
        quicklistDecompressNode(t);
        quicklistDecompressNode(tp);
        if (h != node && hn != node && t != node && tp != node) {
            quicklistCompressNode(node); ///如果上面4个节点都不是我们要查找的节点，就对node进行压缩操作
        }
        if (hnn != t) {
            quicklistCompressNode(hnn); ///如果第三个节点不是尾节点，将其进行压缩操作
        }
        if (tpp != h) {
            quicklistCompressNode(tpp); ///如果倒数第三个节点不是头节点，将其进行压缩操作
        }
        return;
    }
#endif

    /* Iterate until we reach compress depth for both sides of the list.a
     * Note: because we do length checks at the *top* of this function,
     *       we can skip explicit null checks below. Everything exists. */
    ///从两端向中间进行压缩操作
    quicklistNode *forward = quicklist->head;
    quicklistNode *reverse = quicklist->tail;
    int depth = 0;
    int in_depth = 0;
    while (depth++ < quicklist->compress) { ///如果depth小于quicklist->compress，表示该节点不用进行压缩
        quicklistDecompressNode(forward); ///如果需要进行解压操作，就进行解压缩操作
        quicklistDecompressNode(reverse); 

        if (forward == node || reverse == node) ///如果找node节点,就设置标志
            in_depth = 1;

        if (forward == reverse) ///如果头尾指针相遇，直接返回
            return;

        forward = forward->next; ///指向后继节点
        reverse = reverse->prev; ///指向前驱节点
    }

    if (!in_depth)  ///如果node不在两端不需要压缩的的范围，就表示需要就这个节点进行压缩
        quicklistCompressNode(node);

    if (depth > 2) { ///如果深度大于2，需要压缩首尾两个指针指向的节点
        /* At this point, forward and reverse are one node beyond depth */
        quicklistCompressNode(forward);
        quicklistCompressNode(reverse);
    }
}

///如果node可以进行压缩操作，就将其压缩，否则，就通过__quicklistCompress()方法范围压缩，
#define quicklistCompress(_ql, _node)                                          \
    do {                                                                       \
        if ((_node)->recompress)                                               \
            quicklistCompressNode((_node));                                    \
        else                                                                   \
            __quicklistCompress((_ql), (_node));                               \
    } while (0)

/* If we previously used quicklistDecompressNodeForUse(), just recompress. */
///如果我们之前有调用quicklistDecompressNodeForUse()，就需要重新进行再次压缩
#define quicklistRecompressOnly(_ql, _node)                                    \
    do {                                                                       \
        if ((_node)->recompress)                                               \
            quicklistCompressNode((_node));                                    \
    } while (0)

///如果after = 1， 表示在old_node节点后面插入新的节点
///如果after = 0， 表示在old_node节点前面插入新的节点
///新的节点是没有压缩过的，如果我们直接将其设置尾头节点和尾节点，是不用进行压缩操作
REDIS_STATIC void __quicklistInsertNode(quicklist *quicklist,
                                        quicklistNode *old_node,
                                        quicklistNode *new_node, int after) {
    if (after) {
        new_node->prev = old_node; ///在old_node后面插入new_node节点
        if (old_node) { ///如果old_node存在
            new_node->next = old_node->next; ///将new_node的next指针做修改
            if (old_node->next) ///如果old_node的后继节点存在，就修改它的prev指针
                old_node->next->prev = new_node;
            old_node->next = new_node; ///修改old_node->next的值
        } 
        if (quicklist->tail == old_node) ///如果old_node是尾节点
            quicklist->tail = new_node; ///则直接将新的尾节点指向new_node
    } else {  
        new_node->next = old_node; ///在old_node的前面插入new_node节点
        if (old_node) {
            new_node->prev = old_node->prev;
            if (old_node->prev)
                old_node->prev->next = new_node;
            old_node->prev = new_node;
        }
        if (quicklist->head == old_node)
            quicklist->head = new_node; /// 如果old_node是头节点，则直接修改快表的头节点指针，指向new_node
    }
    /* If this insert creates the only element so far, initialize head/tail. */
    if (quicklist->len == 0) { ///如果插入之前是一个空的快表
        quicklist->head = quicklist->tail = new_node; ///将快表的头节点指针和尾节点指针指向new_node
    }

    if (old_node) ///
        quicklistCompress(quicklist, old_node); ///对old_node进行压缩操作

    quicklist->len++;
}

/* Wrappers for node inserting around existing node. */
///在old_node前面插入节点
REDIS_STATIC void _quicklistInsertNodeBefore(quicklist *quicklist,
                                             quicklistNode *old_node,
                                             quicklistNode *new_node) {
    __quicklistInsertNode(quicklist, old_node, new_node, 0);
}

///在old_node后面插入节点
REDIS_STATIC void _quicklistInsertNodeAfter(quicklist *quicklist,
                                            quicklistNode *old_node,
                                            quicklistNode *new_node) {
    __quicklistInsertNode(quicklist, old_node, new_node, 1);
}

///判断快表节点的压缩表的大小是否满足fill的要求
REDIS_STATIC int
_quicklistNodeSizeMeetsOptimizationRequirement(const size_t sz,
                                               const int fill) {
    if (fill >= 0)
        return 0;

    ///static const size_t optimization_level[] = {4096, 8192, 16384, 32768, 65536};
    size_t offset = (-fill) - 1; ///fill小于0，offset表示它在数组中的偏移量
    if (offset < (sizeof(optimization_level) / sizeof(*optimization_level))) {
        if (sz <= optimization_level[offset]) { ///算出fill的等级，如果sz小于对应等级的大小，则返回1，表示满足要求
            return 1;
        } else {
            return 0;
        }
    } else {
        return 0;
    }
}

///判断sz大小是否超过了SIZE_SAFETY_LIMIT = 8192，
#define sizeMeetsSafetyLimit(sz) ((sz) <= SIZE_SAFETY_LIMIT)

///判断node节点是否能插入到快表中，主要通过fill和size两个指标进行判断
REDIS_STATIC int _quicklistNodeAllowInsert(const quicklistNode *node,
                                           const int fill, const size_t sz) {
    if (unlikely(!node)) ///unlikely()表示为假可能性的函数，如果!node为假的可能性大，就直接返回
        return 0;

    int ziplist_overhead; ///
    /* size of previous offset */
    if (sz < 254) ///上一个节点的信息
        ziplist_overhead = 1; ///如果压缩表的大小小于254，需要一个字节编码
    else
        ziplist_overhead = 5; ///否则需要5个字节来编码

    /* size of forward offset */
    if (sz < 64) ///当前节点的信息
        ziplist_overhead += 1; ///如果长度小于64，用一个字节的编码
    else if (likely(sz < 16384)) ///如果小于16384，就用两个字节编码
        ziplist_overhead += 2;
    else                         ///否则，用5个字节编码
        ziplist_overhead += 5; 

    /* new_sz overestimates if 'sz' encodes to an integer type */
    ///新节点的长度为当前节点长度 + 压缩表节点大小+压缩表当前节点编码长度 + 压缩表前一个节点编码长度
    unsigned int new_sz = node->sz + sz + ziplist_overhead;
    if (likely(_quicklistNodeSizeMeetsOptimizationRequirement(new_sz, fill))) ///如果new_符合fill要求
        return 1; ///返回成功
    else if (!sizeMeetsSafetyLimit(new_sz)) ///如果new_sz的大小超过了安全线，返回0
        return 0;
    else if ((int)node->count < fill) ///如果节点中压缩表节点数小于fill的要求，返回1 
        return 1;
    else
        return 0;
}

///根据fill来判断两个快表节点中的压缩表是否可以进行合并操作，如果可以则返回1，否则返回0
REDIS_STATIC int _quicklistNodeAllowMerge(const quicklistNode *a,
                                          const quicklistNode *b,
                                          const int fill) {
    if (!a || !b) ///如果两个快表中有一个为空，则直接返回0
        return 0;

    /* approximate merged ziplist size (- 11 to remove one ziplist
     * header/trailer) */
    ///计算连个压缩表的大小，但是要减去一个表头和结尾标志符的大小（4(albytes) + 4(zltail_offset) + 2(zllength) + 1(zlend) = 11）
    unsigned int merge_sz = a->sz + b->sz - 11;
    if (likely(_quicklistNodeSizeMeetsOptimizationRequirement(merge_sz, fill))) ///如果merge_sz符合要求，返回1
        return 1;
    else if (!sizeMeetsSafetyLimit(merge_sz)) ///如果merge_sz大小超过了安全限定，返回0
        return 0;
    else if ((int)(a->count + b->count) <= fill) ///如果两个压缩表的节点总数符合要求，返回1
        return 1;
    else
        return 0;
}

///更新快表节点中的压缩表大小
#define quicklistNodeUpdateSz(node)                                            \
    do {                                                                       \
        (node)->sz = ziplistBlobLen((node)->zl);                               \
    } while (0)

/* Add new entry to head node of quicklist.
 *
 * Returns 0 if used existing head.
 * Returns 1 if new head created. */
///在头节点中添加新的ziplist，返回0表示用原来头节点，返回1表示改变头节点指针指向新节点
int quicklistPushHead(quicklist *quicklist, void *value, size_t sz) {
    
    quicklistNode *orig_head = quicklist->head; ///获取快表的头节点
    if (likely(
            _quicklistNodeAllowInsert(quicklist->head, quicklist->fill, sz))) { ///检测是否能够插入这个节点
        quicklist->head->zl =
            ziplistPush(quicklist->head->zl, value, sz, ZIPLIST_HEAD); ///将这个ziplist以头插入的方式插入快表头节点的ziplist中
        quicklistNodeUpdateSz(quicklist->head); ///更新快表head节点中的ziplist数据记录
    } else { ///如果插入新的entry后不满足file，size的约定
        quicklistNode *node = quicklistCreateNode(); ///就需要创建一个新的快表节点 
        node->zl = ziplistPush(ziplistNew(), value, sz, ZIPLIST_HEAD);///将这个ziplist写入到这个新的节点中

        quicklistNodeUpdateSz(node); ///更新这个新建节点中关于ziplist的信息
        _quicklistInsertNodeBefore(quicklist, quicklist->head, node); ///将这个节点插入到快表头节点之前，称为新的头节点
    }
    quicklist->count++; ///修改快表中压缩表节点的数目
    quicklist->head->count++; ///修改头节点中的压缩表的节点数目
    return (orig_head != quicklist->head);  ///如果头节点发生改变，就返回1，否则返回0
}

/* Add new entry to tail node of quicklist.
 *
 * Returns 0 if used existing tail.
 * Returns 1 if new tail created. */
 ///在快表的尾节点插入一个新的ziplist实体，它的操作和在头部插入一样，这里不再进行详细解释了
int quicklistPushTail(quicklist *quicklist, void *value, size_t sz) {
    quicklistNode *orig_tail = quicklist->tail;
    if (likely(
            _quicklistNodeAllowInsert(quicklist->tail, quicklist->fill, sz))) {
        quicklist->tail->zl =
            ziplistPush(quicklist->tail->zl, value, sz, ZIPLIST_TAIL);
        quicklistNodeUpdateSz(quicklist->tail);
    } else {
        quicklistNode *node = quicklistCreateNode();
        node->zl = ziplistPush(ziplistNew(), value, sz, ZIPLIST_TAIL);

        quicklistNodeUpdateSz(node);
        _quicklistInsertNodeAfter(quicklist, quicklist->tail, node);
    }
    quicklist->count++;
    quicklist->tail->count++;
    return (orig_tail != quicklist->tail);
}

/* Create new node consisting of a pre-formed ziplist.
 * Used for loading RDBs where entire ziplists have been stored
 * to be retrieved later. */
/// 创建一个新的节点，该节点由预先形成的ziplist组成。 用于加载已存储整个ziplist以便以后检索的RDB。 
void quicklistAppendZiplist(quicklist *quicklist, unsigned char *zl) {
    quicklistNode *node = quicklistCreateNode(); ///创建一个新的快表节点

    node->zl = zl; ///将node的ziplist指针指向这个给定的zl
    node->count = ziplistLen(node->zl); ///初始化节点中压缩表的节点数目
    node->sz = ziplistBlobLen(zl); ///初始化节点中压缩表的大小

    _quicklistInsertNodeAfter(quicklist, quicklist->tail, node); ///将这个节点插入到快表的尾部
    quicklist->count += node->count; ///更新快表中记录压缩表的大小的数据
}

/* Append all values of ziplist 'zl' individually into 'quicklist'.
 *
 * This allows us to restore old RDB ziplists into new quicklists
 * with smaller ziplist sizes than the saved RDB ziplist.
 * 这使我们可以将旧的RDB ziplist还原到新的快速列表中，而列表的大小比保存的RDB ziplist小。
 *
 * Returns 'quicklist' argument. Frees passed-in ziplist 'zl' */
///将ziplist实体添加到quicklist中
quicklist *quicklistAppendValuesFromZiplist(quicklist *quicklist,
                                            unsigned char *zl) {
    unsigned char *value;
    unsigned int sz;
    long long longval;
    char longstr[32] = {0}; 

    unsigned char *p = ziplistIndex(zl, 0); ///找到ziplist下标为0的节点地址
    while (ziplistGet(p, &value, &sz, &longval)) { ///获取p指向节点的值，可能是字符串也可能是整数
        if (!value) { ///如果值为空的字符串, 那么value就整数类型
            sz = ll2string(longstr, sizeof(longstr), longval); ///将long long类型的整数转化为字符串
            value = (unsigned char *)longstr; ///并将这个字符串保存到value中
        }
        quicklistPushTail(quicklist, value, sz); ///将这个ziplist加入到快表的尾节点中
        p = ziplistNext(zl, p);  ///p指向下一个ziplist节点 
    }
    zfree(zl);  /// 释放原来的ziplist
    return quicklist; ///返回修改后的快表
}

///创建一个快表，并经ziplist加入其中
quicklist *quicklistCreateFromZiplist(int fill, int compress,unsigned char *zl) {
   
    ///这个函数就是县创建一个空的快表表，然后将zl加入到这个快表中
    return quicklistAppendValuesFromZiplist(quicklistNew(fill, compress), zl);
}

///从快表中删除节点n
#define quicklistDeleteIfEmpty(ql, n)                                          \
    do {                                                                       \
        if ((n)->count == 0) {     ///如果n的压缩表大小为0                        \
            __quicklistDelNode((ql), (n));   ///只需要删除这个节点即可             \
            (n) = NULL;                                                        \
        }                                                                      \
    } while (0)

///从快表中删除节点node        
REDIS_STATIC void __quicklistDelNode(quicklist *quicklist,
                                     quicklistNode *node) {
    /* Update the bookmark if any */
    quicklistBookmark *bm = _quicklistBookmarkFindByNode(quicklist, node); ///如果node是一个bookmark，需要进行更新
    if (bm) {
        bm->node = node->next; ///更新bookmark中的node指针
        /* if the bookmark was to the last node, delete it. */
        if (!bm->node) ///如果bookmark指向的node为最后一个节点，就删除这个bookmark
            _quicklistBookmarkDelete(quicklist, bm); 
    }

    if (node->next) ///删除节点，调整节点指针
        node->next->prev = node->prev;
    if (node->prev)
        node->prev->next = node->next;

    if (node == quicklist->tail) { ///如果删除节点是尾节点
        quicklist->tail = node->prev;
    }

    if (node == quicklist->head) { ///如果删除的节点为头节点
        quicklist->head = node->next;
    }

    ///如果我们删除的节点在压缩范围内，则现在需要解压缩的压缩节点。 
    __quicklistCompress(quicklist, NULL);

    quicklist->count -= node->count; //更新快表中压缩表节点数量

    zfree(node->zl); ///释放压缩表
    zfree(node); ///释放节点
    quicklist->len--; ///快表中的节点数量-1
}

///从快表中的node节点中删除压缩表节点p，如果删除后该快表节点的压缩表节点数为0，就需要删除这个快表节点。
/// 如果删除了快表节点，返回1，否则返回0
REDIS_STATIC int quicklistDelIndex(quicklist *quicklist, quicklistNode *node,
                                   unsigned char **p) {
    int gone = 0;

    node->zl = ziplistDelete(node->zl, p); ///删除node节点的ziplist中的p
    node->count--; ///快表节点中记录压缩表节点数的计算器-1
    if (node->count == 0) { ///如果删除压缩表节点后node的count变成了0
        gone = 1;
        __quicklistDelNode(quicklist, node); ///就需要从快表中删除节点
    } else {
        quicklistNodeUpdateSz(node); ///否则就更新快表中的node节点
    }
    quicklist->count--; ///快表中的压缩表节点计数器-1
    /* If we deleted the node, the original node is no longer valid */
    return gone ? 1 : 0; ///如果删除快表节点，就返回1，否则返回0
}

/* Delete one element represented by 'entry'
 *
 * 'entry' stores enough metadata to delete the proper position in
 * the correct ziplist in the correct quicklist node. */
///删除一个entry，
void quicklistDelEntry(quicklistIter *iter, quicklistEntry *entry) {
    
    quicklistNode *prev = entry->node->prev; ///获取快表的头指针
    quicklistNode *next = entry->node->next; ///获取快表的尾指针
    ///获取quicklistEntry实体entry的所属快表，所属快表节点，以及ziplist指针的位置，
    ///然后通过调用quicklistDelIndex()方法完成对ziplist进行删除操作
    int deleted_node = quicklistDelIndex((quicklist *)entry->quicklist,
                                         entry->node, &entry->zi);

    /* after delete, the zi is now invalid for any future usage. */
    iter->zi = NULL; ///更新迭代器

    if (deleted_node) { ///如果当前的快表节点已经被删除掉了，x与奥更新迭代器指向的位置
        if (iter->direction == AL_START_HEAD) { ///如果是从头到尾进行遍历
            iter->current = next; ///让迭代器指向下一个节点
            iter->offset = 0;
        } else if (iter->direction == AL_START_TAIL) { ///如果是从尾到头进行遍历
            iter->current = prev; ///就让迭代器指向前一个节点，offset设置为-1
            iter->offset = -1;
        }
    }
    ///如果说着这个快表节点没有被删除，就不用进行迭代器转移操作
    /* else if (!deleted_node), no changes needed.
     * we already reset iter->zi above, and the existing iter->offset
     * doesn't move again because:
     *   - [1, 2, 3] => delete offset 1 => [1, 3]: next element still offset 1
     *   - [1, 2, 3] => delete offset 0 => [2, 3]: next element still offset 0
     *  if we deleted the last element at offet N and now
     *  length of this ziplist is N-1, the next call into
     *  quicklistNext() will jump to the next node. */
    
}

/* Replace quicklist entry at offset 'index' by 'data' with length 'sz'.
 * 将偏移量“index”处的快表节点的entry替换为长度为“sz”的“data”。
 * 如果进行了替换操作就返回1
 * 如果没有进行替换或者替换操作失败就返回0
 */
///将下标为index处的entry用data替换
int quicklistReplaceAtIndex(quicklist *quicklist, long index, void *data, int sz) {
    quicklistEntry entry;
    if (likely(quicklistIndex(quicklist, index, &entry))) { ///首先通过quicklistIndex()找到下标为index处的压缩表节点
        /* quicklistIndex provides an uncompressed node */
        entry.node->zl = ziplistDelete(entry.node->zl, &entry.zi); ///删除需要被替换的压缩表节点
        entry.node->zl = ziplistInsert(entry.node->zl, entry.zi, data, sz); ///在删除的位置插入替换的压缩表节点
        quicklistNodeUpdateSz(entry.node); ///更新快表的头部信息
        quicklistCompress(quicklist, entry.node); ///对该节点按需进行压缩操作
        return 1; ///返回操作成功
    } else { ///否则返回失败
        return 0;
    }
}

/* 给定两个节点，请尝试合并它们的ziplist。
 *
 * 如果我fill可以处理更高级别的内容，这将有助于我们避免使用包含3个元素的ziplist的快速列表。
 *
 * 注意：a必须在b的前面
 *
 * 调用此函数后，“ a”和“ b”均应视为不可用。 必须使用此函数的返回值，而不是重新使用任何quicklistNode输入参数。
 *
 * 返回选择合并的输入节点；如果无法合并，则返回NULL。
 */
///合并两个quicklistNode中的ziplist
REDIS_STATIC quicklistNode *_quicklistZiplistMerge(quicklist *quicklist,
                                                   quicklistNode *a,
                                                   quicklistNode *b) {
    D("Requested merge (a,b) (%u, %u)", a->count, b->count);

    quicklistDecompressNode(a); ///对快表节点a执行解压缩操作
    quicklistDecompressNode(b); ///对快表节点b执行解压缩操作
    if ((ziplistMerge(&a->zl, &b->zl))) { ///合并两个压缩表
        /* We merged ziplists! Now remove the unused quicklistNode. */
        quicklistNode *keep = NULL, *nokeep = NULL;
        if (!a->zl) { ///如果a指向的zl为空，说明a已经发生了变化
            nokeep = a; 
            keep = b;
        } else if (!b->zl) { ///如果b指向的zl为空，则说明a已经发生了变化
            nokeep = b;
            keep = a;
        }
        keep->count = ziplistLen(keep->zl); ///更新快表节点中的压缩表的数量
        quicklistNodeUpdateSz(keep); ///更新快表中记录压缩表节点数目的值
 
        nokeep->count = 0; ///将a合并到b中，所以将a的数量置为0
        __quicklistDelNode(quicklist, nokeep); ///删除合并后没有不在使用的节点
        quicklistCompress(quicklist, keep); ///按需压缩保留的节点
        return keep;  ///返回保留的节点
    } else {
        return NULL; ///如果操作失败或者没进行任何操作，返回NULL
    }
}

/* 尝试合并以center节点为中心的左右两个节点，最多将左右4个节点合并到center上（5个节点合成1个节点）
 *
 * 尝试合并的节点情况:
 *   - (center->prev->prev, center->prev)
 *   - (center->next, center->next->next)
 *   - (center->prev, center)
 *   - (center, center->next)
 */
REDIS_STATIC void _quicklistMergeNodes(quicklist *quicklist,
                                       quicklistNode *center) {
    
    int fill = quicklist->fill; ///获取快表的fill参数
    quicklistNode *prev, *prev_prev, *next, *next_next, *target; ///声明5个节点指针
    prev = prev_prev = next = next_next = target = NULL;

    if (center->prev) { ///如果center节点的前驱节点存在
        prev = center->prev; ///获取前驱节点
        if (center->prev->prev) ///如果center的前驱节点的前驱节点也存在
            prev_prev = center->prev->prev; ///获取它
    }

    if (center->next) { ///如果center的后继节点存在
        next = center->next;
        if (center->next->next) ///如果center后继的后继存在
            next_next = center->next->next;
    }

    ///尝试合并前驱节点和前驱的前驱节点
    if (_quicklistNodeAllowMerge(prev, prev_prev, fill)) { ///判断是否满足fill的要求
        _quicklistZiplistMerge(quicklist, prev_prev, prev); ///满足要求则进行合并
        prev_prev = prev = NULL; ///合并后将这两个节点置空
    }

    ///尝试合并后继以及后继的后继节点
    if (_quicklistNodeAllowMerge(next, next_next, fill)) { ///判断是否满足fill的要求
        _quicklistZiplistMerge(quicklist, next, next_next); ///满足要求则进行合并
        next = next_next = NULL; ///合并后将这两个节点置空
    }

    ///将center节点和它的前驱节点合并
    if (_quicklistNodeAllowMerge(center, center->prev, fill)) { ///如果满足fill要求
        target = _quicklistZiplistMerge(quicklist, center->prev, center); ///将两个节点合并
        center = NULL; ///将center节点置空
    } else {
        /* 如果不能合并，我们必须保证目标在下面必须有效。 */
        target = center;
    }

    /* 使用我们合并后的中心节点（或者没有合并的中心节点）与下一个节点进行合并操作*/
    if (_quicklistNodeAllowMerge(target, target->next, fill)) { ///判断是否满足合并的要求
        _quicklistZiplistMerge(quicklist, target, target->next); ///如果满足就进行合并操作
    }
}

/* 通过参数offset和after将node划分为两个部分“after”参数控制返回哪个quicklistNode。
 *
 * If 'after'==1, returned node has elements after 'offset'.
 *                input node keeps elements up to 'offset', including 'offset'.
 * If 'after'==0, returned node has elements up to 'offset', including 'offset'.
 *                input node keeps elements after 'offset'.
 *
 * If 'after'==1, returned node will have elements _after_ 'offset'.
 *                The returned node will have elements [OFFSET+1, END].
 *                The input node keeps elements [0, OFFSET].
 *
 * If 'after'==0, returned node will keep elements up to and including 'offset'.
 *                The returned node will have elements [0, OFFSET].
 *                The input node keeps elements [OFFSET+1, END].
 *
 * The input node keeps all elements not taken by the returned node.
 *
 * Returns newly created node or NULL if split not possible. 
 */
REDIS_STATIC quicklistNode *_quicklistSplitNode(quicklistNode *node, int offset,
                                                int after) {
    size_t zl_sz = node->sz; ///获取快表节点的压缩表大小

    quicklistNode *new_node = quicklistCreateNode(); ///创建一个新出的压缩表节点
    new_node->zl = zmalloc(zl_sz); //申请zl_sz大小的内存个空间来存在压缩表

    /* Copy original ziplist so we can split it */
    memcpy(new_node->zl, node->zl, zl_sz);  ///将原来的压缩表拷贝到这个新的节点中

    /* -1 here means "continue deleting until the list ends" */
    /// -1表示压缩表最后一个节点的下标
    int orig_start = after ? offset + 1 : 0; ///如果after = 1，那么orig_start = offset + 1, 否则就为0
    int orig_extent = after ? -1 : offset; ///如果after = 1， orig_extent = -1，反之等于offset
    int new_start = after ? 0 : offset;
    int new_extent = after ? offset + 1 : -1;
    ///总结：after = 1，表示将node分成两个部分，new部分位于org部分之前
    ///     after = 0，表示将node分成两个部分，new部分位于org部分之后
    D("After %d (%d); ranges: [%d, %d], [%d, %d]", after, offset, orig_start,
      orig_extent, new_start, new_extent);

    node->zl = ziplistDeleteRange(node->zl, orig_start, orig_extent); //将原来节点中的org部分删除
    node->count = ziplistLen(node->zl); ///更新节点的压缩表节点计数器
    quicklistNodeUpdateSz(node); ///更新快表节点关于压缩表的信息

    new_node->zl = ziplistDeleteRange(new_node->zl, new_start, new_extent); ///删除新节点中new部分
    new_node->count = ziplistLen(new_node->zl);///更新新节点的压缩表计数器
    quicklistNodeUpdateSz(new_node);///更新快表节点表关于压缩表的信息

    D("After split lengths: orig (%d), new (%d)", node->count, new_node->count);
    return new_node;
}

/* 在快表中的quicklistEntry实体前面或者后面插入一个新的entry
 * 如果 after==1, 表示在已经存在的entry后面插入新的entry，否则就在已经存在entry前面插入一个entry
 */
REDIS_STATIC void _quicklistInsert(quicklist *quicklist, quicklistEntry *entry,
                                   void *value, const size_t sz, int after) {
    
    int full = 0, at_tail = 0, at_head = 0, full_next = 0, full_prev = 0;
    int fill = quicklist->fill; ///获取快表的fill参数
    quicklistNode *node = entry->node; ///获取快表节点
    quicklistNode *new_node = NULL; ///创建一个新节点的指针

    if (!node) { ///如果entry中没有node这个属性，就需要创建节点属性
        /* we have no reference node, so let's create only node in the list */
        D("No node given!");
        new_node = quicklistCreateNode(); ///创建一个新的快表节点
        new_node->zl = ziplistPush(ziplistNew(), value, sz, ZIPLIST_HEAD); ///将新的压缩表信息加入到新节点的ziplist中
        __quicklistInsertNode(quicklist, NULL, new_node, after); ////在快表中插入新节点
        new_node->count++; ///修改节点的计数器
        quicklist->count++; ///修改快表的计数器
        return;
    }

    /* Populate accounting flags for easier boolean checks later */
    if (!_quicklistNodeAllowInsert(node, fill, sz)) { ///如果插入的entry不满足fill的要求
        D("Current node is full with count %d with requested fill %lu",
          node->count, fill);
        full = 1; ///设置full标志
    }

    if (after && (entry->offset == node->count)) { ///如果after = 1并且entry的偏移量和node的计数相等
        D("At Tail of current ziplist");
        at_tail = 1; ///设置at_tail标志
        if (!_quicklistNodeAllowInsert(node->next, fill, sz)) { ///判断插入的节点是否满足fill要求
            D("Next node is full too.");
            full_next = 1; ///如果不满足，设置full_next标志
        }
    }

    if (!after && (entry->offset == 0)) {///如果after = 0并且entry偏移量为0
        D("At Head");
        at_head = 1; ///设置at_head标志
        if (!_quicklistNodeAllowInsert(node->prev, fill, sz)) { ///判断是否满足fill要求
            D("Prev node is full too.");
            full_prev = 1; ///如果不满足，设置full_prev标志
        }
    }

    ///现在通过上面的标志为，确定是在那个地方以及怎么插入这个新的节点
    ///如果满足fill要求并且是采用后插入操作
    if (!full && after) {
        D("Not full, inserting after current position.");
        quicklistDecompressNodeForUse(node); ///将node进行解压操作
        unsigned char *next = ziplistNext(node->zl, entry->zi); ///返回node->zl锁指向的下一个压缩表节点的位置
        if (next == NULL) { ///如果下一个节点为空，表示该节点已经为尾节点 
            node->zl = ziplistPush(node->zl, value, sz, ZIPLIST_TAIL);///直接在尾部插入即可
        } else {
            node->zl = ziplistInsert(node->zl, next, value, sz); ///否则就在entry的后面插入一个压缩表节点
        }
        node->count++; ///将快表节点的压缩表节点计数器+1
        quicklistNodeUpdateSz(node); ///更新快表节点的压缩表大小信息
        quicklistRecompressOnly(quicklist, node); ///对node进行重压缩
    } else if (!full && !after) { ///如果fill满足要求，在entry的前面插入一个压缩表节点
        D("Not full, inserting before current position.");
        quicklistDecompressNodeForUse(node); ///将node解压
        node->zl = ziplistInsert(node->zl, entry->zi, value, sz); ///在entry的前面插入压缩表节点
        node->count++;///将快表节点的压缩表节点计数器+1
        quicklistNodeUpdateSz(node);///更新快表节点的压缩表大小信息
        quicklistRecompressOnly(quicklist, node);//对node进行重压缩
    } 
    ///如果当前的node已经满了（full ==1），并且当前的entry是尾节点，node的next不为空，但是它的next能够没有满
    ///并且采用的是尾插入的形式
    else if (full && at_tail && node->next && !full_next && after) {
        
        D("Full and tail, but next isn't full; inserting next node head");
        new_node = node->next; ///让new_node指向node的下一个节点
        quicklistDecompressNodeForUse(new_node); ///将new_node进行解压缩操作
        new_node->zl = ziplistPush(new_node->zl, value, sz, ZIPLIST_HEAD); ///采用头插入的方式在new_node的压缩表中新增一个压缩表节点
        new_node->count++; ///new_node的压缩表计数器+1
        quicklistNodeUpdateSz(new_node); ///更新new_node的压缩表大小
        quicklistRecompressOnly(quicklist, new_node); ///对new_node进行重压缩
    } 
    ///如果当前节点为full，并且entry已经是头部节点，前驱节点没有满，而且采用的是头插入的方式
    else if (full && at_head && node->prev && !full_prev && !after) {
        
        D("Full and head, but prev isn't full, inserting prev node tail");
        new_node = node->prev; ///让new_node指向node的前驱节点
        quicklistDecompressNodeForUse(new_node); ///将new_node进行解压缩操作
        new_node->zl = ziplistPush(new_node->zl, value, sz, ZIPLIST_TAIL); ///采用尾插入的形式在new_node的尾部插入一个压缩表节点
        new_node->count++; ///new_node的压缩表计数器+1
        quicklistNodeUpdateSz(new_node);///更新new_node的压缩表大小
        quicklistRecompressOnly(quicklist, new_node);///对new_node进行重压缩
    } 
    ///上面集中情况是比较好的，下面这种情况就比较糟糕，我们必须要创建新的节点才行。
    ///如果上面几种情况不成立
    else if (full && ((at_tail && node->next && full_next && after) ||
                        (at_head && node->prev && full_prev && !after))) {
      
        D("\tprovisioning new node...");
        new_node = quicklistCreateNode(); ///创建一个新的节点
        new_node->zl = ziplistPush(ziplistNew(), value, sz, ZIPLIST_HEAD);  ///将entry插入到new_node的头部
        new_node->count++; ///更新new_node的压缩表节点计数器
        quicklistNodeUpdateSz(new_node); ///更新new_node的压缩表大小
        __quicklistInsertNode(quicklist, node, new_node, after); ///将new_node插入当前node的后面
    } 
    ///如果当前node满来，且将entry插入在中间的位置，需要将node分割
    else if (full) {
      
        D("\tsplitting node...");
        quicklistDecompressNodeForUse(node); ///对node进行解压操作
        new_node = _quicklistSplitNode(node, entry->offset, after); ///将node进行切割操作=
        new_node->zl = ziplistPush(new_node->zl, value, sz,
                                   after ? ZIPLIST_HEAD : ZIPLIST_TAIL); ///将entry加入到new_node中去
        new_node->count++; ///更新new_node的压缩表节点计数
        quicklistNodeUpdateSz(new_node); ///更新new_node压缩表大小
        __quicklistInsertNode(quicklist, node, new_node, after); ///在node后面插入new_node
        _quicklistMergeNodes(quicklist, node); ///node进行左右合并
    }

    quicklist->count++; ///更新快表的压缩表节点计数 +1
}

///在entry节点处进行前插入
void quicklistInsertBefore(quicklist *quicklist, quicklistEntry *entry,
                           void *value, const size_t sz) {
    _quicklistInsert(quicklist, entry, value, sz, 0);
}

///在entry节点处进行后插入
void quicklistInsertAfter(quicklist *quicklist, quicklistEntry *entry,
                          void *value, const size_t sz) {
    _quicklistInsert(quicklist, entry, value, sz, 1);
}

/* 从快速列表中删除一系列元素。
 * 注意：：：：元素可能跨越多个quicklistNodes，因此我们必须谨慎跟踪起点和终点。
 * 如果删除了条目，则返回1；如果未删除任何内容，则返回0。 
 */
int quicklistDelRange(quicklist *quicklist, const long start, const long count) {
    
    if (count <= 0) ///如果删除的元素个数小于或者等于0，则直接返回
        return 0;

    unsigned long extent = count; 
    ///如果开始的位置大于等于0，并且要删除的元素个数大于快表中的元素个数-开始位置
    if (start >= 0 && extent > (quicklist->count - start)) { 
        
        extent = quicklist->count - start;  ///更新需要删除的元素个数的计数器
    } 
    ///如果开始元素小于0，表示从尾部开始进行删除操作，如果删除元素个数计数器大于-start，则删除到尾部即可
    else if (start < 0 && extent > (unsigned long)(-start)) {
           
        extent = -start; ///更新extent的数值
    }

    quicklistEntry entry;
    if (!quicklistIndex(quicklist, start, &entry)) ///找到start位置的压缩表的地址
        return 0; ///如果为空，则直接返回

    D("Quicklist delete request for start %ld, count %ld, extent: %ld", start,
      count, extent);
    quicklistNode *node = entry.node; ///获取entry所在的快表节点

    /* iterate over next nodes until everything is deleted. */
    while (extent) { ///迭代，进行数据删除 
        quicklistNode *next = node->next; ///获取节点的下一个节点，主要是为了防止当前节点中的压缩表数量不够，需要进行跨多个节点

        unsigned long del;
        int delete_entire_node = 0; /// 用来要删除的节点数量
        if (entry.offset == 0 && extent >= node->count) { ///如果entry偏移量为0并且当前节点中的压缩表元素个数小于要删除的节点个数
           
            delete_entire_node = 1; ///删除节点数量置为1
            del = node->count; ///更新已删除压缩表节点的数量
        } else if (entry.offset >= 0 && extent >= node->count) { ///如果entry的offset大于0并且当前要删除的压缩表节点数大约node的压缩表的节点数量的
            
            del = node->count - entry.offset;   /// 更新已删除压缩表节点的数量
        } else if (entry.offset < 0) { ///如果entry的offset小于0.则从尾节点向前删除offset个节点
            /* If offset is negative, we are in the first run of this loop
             * and we are deleting the entire range
             * from this start offset to end of list.  Since the Negative
             * offset is the number of elements until the tail of the list,
             * just use it directly as the deletion count. */
            del = -entry.offset; ///更新删除压缩表的节点数

            /* If the positive offset is greater than the remaining extent,
             * we only delete the remaining extent, not the entire offset.
             */
            if (del > extent)  ///这里控制删除压缩表节点的数量， 最多只能删除count个
                del = extent;
        } else {
            del = extent;
        }

        D("[%ld]: asking to del: %ld because offset: %d; (ENTIRE NODE: %d), "
          "node count: %u",
          extent, del, entry.offset, delete_entire_node, node->count);

        if (delete_entire_node) { ///如果删除节点的标志符为真，表明需要删除这快表个节点
            __quicklistDelNode(quicklist, node);  ///进行快表节点删除
        } else {///如果不用删除快表节点
            quicklistDecompressNodeForUse(node); ///解压节点
            node->zl = ziplistDeleteRange(node->zl, entry.offset, del); ///删除节点中一定范围的压缩表节点
            quicklistNodeUpdateSz(node); ///更新node的压缩表大小
            node->count -= del; ///更新快表节点node的压缩表节点计数
            quicklist->count -= del; ///更新快表的压缩表节点计数
            quicklistDeleteIfEmpty(quicklist, node); ///如果压缩表为空，就需要删除这个节点
            if (node) ///如果node不会空
                quicklistRecompressOnly(quicklist, node); ///对node进行重压缩
        }
        extent -= del; ///修改删除节点数量的计数器
        node = next; ///指向下一个节点
        entry.offset = 0;///重置entry的offset
    }
    return 1;
}

/* Passthrough to ziplistCompare() */
///比较两个压缩表，将ziplist中的ziplistCompare()函数封装成quicklistCompare函数
int quicklistCompare(unsigned char *p1, unsigned char *p2, int p2_len) {
    return ziplistCompare(p1, p2, p2_len);
}
///返回快表迭代器“iter”。 初始化后，每次对quicklistNext()的调用都将返回快表的下一个元素。
quicklistIter *quicklistGetIterator(const quicklist *quicklist, int direction) {
  
    quicklistIter *iter;
    iter = zmalloc(sizeof(*iter)); ///为迭代器声明内存空间

    if (direction == AL_START_HEAD) { ///如果是从头向尾开始遍历
        iter->current = quicklist->head; ///设置当前节点为头节点
        iter->offset = 0; ///设置偏移量
    } else if (direction == AL_START_TAIL) { ///如果是头尾到头的方式遍历
        iter->current = quicklist->tail; ///设置当前节点为尾节点
        iter->offset = -1; ///设置偏移量
    }

    iter->direction = direction; ///设置遍历方向
    iter->quicklist = quicklist; ///设置所属的快表
    iter->zi = NULL; ///初始化ziplist
    return iter;
}

///返回当前位置的迭代器
quicklistIter *quicklistGetIteratorAtIdx(const quicklist *quicklist,
                                         const int direction,
                                         const long long idx) {
    quicklistEntry entry;
    if (quicklistIndex(quicklist, idx, &entry)) { ///将下标为idx处的节点信息读取entry中
        quicklistIter *base = quicklistGetIterator(quicklist, direction); ///生成一个迭代器
        base->zi = NULL; ///设置迭代器的ziplist
        base->current = entry.node;    ///设置迭代器指向的节点
        base->offset = entry.offset;   ///设置迭代器的偏移量
        return base;                   ///返回迭代器
    } else {
        return NULL;
    }
}

///释放迭代器
void quicklistReleaseIterator(quicklistIter *iter) {
    
    if (iter->current) ///如果这个迭代器已经和一个快表节点结合了
        quicklistCompress(iter->quicklist, iter->current); ///根据需要压缩节点

    zfree(iter); ///释放迭代器
}

/* 通过迭代器获取下一个元素
 *
 * 注意：在迭代列表时，请勿插入列表。使用quicklistDelEntry（）函数进行迭代时，可以*从列表中删除。 如果在迭代时插入快速列表，
 * 则添加后应重新创建迭代器。
 *
 * iter = quicklistGetIterator(quicklist,<direction>);
 * quicklistEntry entry;
 * while (quicklistNext(iter, &entry)) {
 *     if (entry.value)
 *          [[ use entry.value with entry.sz ]]
 *     else
 *          [[ use entry.longval ]]
 * }
 *
 * Populates 'entry' with values for this iteration.
 * 当迭代器无法完成迭代时，返回0。如果返回值为0，表示‘entry’的内容无效
 */
int quicklistNext(quicklistIter *iter, quicklistEntry *entry) {
    initEntry(entry); ///初始化快表节点

    if (!iter) { ///如果iter为空，直接返回0
        D("Returning because no iter!");
        return 0;
    }

    entry->quicklist = iter->quicklist; ///设置entry所属的快表
    entry->node = iter->current; ///设置entry所属的快表节点

    if (!iter->current) { 
        D("Returning because current node is NULL")
        return 0;
    }
    
    /// 这是一个函数指针，它的返回值和两个参数都是unsigned char *类型
    unsigned char *(*nextFn)(unsigned char *, unsigned char *) = NULL;
    int offset_update = 0; ///用来记录偏移量

    if (!iter->zi) { ///如果迭代器的ziplist为空
        
        quicklistDecompressNodeForUse(iter->current); ///临时解压缩current节点
        iter->zi = ziplistIndex(iter->current->zl, iter->offset); ///j解压缩后就可以得到zl，以及偏移量
    } else {
        /* else, use existing iterator offset and get prev/next as necessary. */
        if (iter->direction == AL_START_HEAD) { ///如果遍历的方向是从头到尾的方式
            nextFn = ziplistNext; ///nextFn就设置为ziplistNext
            offset_update = 1; ///向后移动一个，记录偏移量
        } else if (iter->direction == AL_START_TAIL) { ///如果是从尾向头的方式遍历
            nextFn = ziplistPrev; ///next就设置为ziplistPrev
            offset_update = -1; ///向前移动一步，并记录下来
        }
        iter->zi = nextFn(iter->current->zl, iter->zi); ///更新zi指向的ziplist中的节点
        iter->offset += offset_update; ///更新迭代器的偏移量
    }

    ///更新快表节点中的信息
    entry->zi = iter->zi; 
    entry->offset = iter->offset;

    if (iter->zi) { ///如果迭代器指向的ziplist不为空
        
        /// 将entry的节点信息读入到快表节点中
        ziplistGet(entry->zi, &entry->value, &entry->sz, &entry->longval);
        return 1;
    } else { ///如果迭代器指向的节点为空
        quicklistCompress(iter->quicklist, iter->current); ///按照需要对节点进行压缩 
        if (iter->direction == AL_START_HEAD) { ///如果是从头到尾进行迭代
           
            D("Jumping to start of next node");
            iter->current = iter->current->next; ///修改迭代器指针
            iter->offset = 0; ///重置偏移量 
        } else if (iter->direction == AL_START_TAIL) { ///如果是从尾到头的方式进行遍历
            /* Reverse traversal */
            D("Jumping to end of previous node");
            iter->current = iter->current->prev; ///修改迭代器指针
            iter->offset = -1; ///修改偏移量
        }
        iter->zi = NULL;
        return quicklistNext(iter, entry); ///递归迭代
    }
}

/* 复制快表
 * 成功后，将返回原始快速列表的副本。
 * 无论成功还是错误，原始的快速列表都不会被修改。
 * 返回新分配的快速列表。
 */
quicklist *quicklistDup(quicklist *orig) {
    
    quicklist *copy; ///声明拷贝副本的指针
    copy = quicklistNew(orig->fill, orig->compress); ///创建一个新的快表

    ///遍历整个全表进行拷贝操作
    for (quicklistNode *current = orig->head; current; current = current->next) {
       
        quicklistNode *node = quicklistCreateNode(); ///创建一个新的快表节点
        if (current->encoding == QUICKLIST_NODE_ENCODING_LZF) { ///如果是采用了LZF算法进行压缩
            quicklistLZF *lzf = (quicklistLZF *)current->zl; ///拷贝quicklistLZF
            size_t lzf_sz = sizeof(*lzf) + lzf->sz; ///获取它的大小
            node->zl = zmalloc(lzf_sz); ///申请内存空间
            memcpy(node->zl, current->zl, lzf_sz); ///进行内容拷贝
        } else if (current->encoding == QUICKLIST_NODE_ENCODING_RAW) { ///如果该节点没有压缩过
            node->zl = zmalloc(current->sz); ///申请内存空间
            memcpy(node->zl, current->zl, current->sz); ///进行节点内容拷贝
        }
        ///更新拷贝节点的头部信息
        node->count = current->count;
        copy->count += node->count;
        node->sz = current->sz;
        node->encoding = current->encoding;
        
        ///在拷贝快表中插入这个节点
        _quicklistInsertNodeAfter(copy, copy->tail, node);
    }
    /* copy->count must equal orig->count here */
    return copy;
}

/* 获取指定下标的元素，其中0是head，1是head旁边的元素，依此类推。 为了从尾数
 * 开始使用负整数，-1是最后一个元素，-2是倒数第二个，依此类推。 如果索引超出范围，则返回0。
 * 
 * 如果返回1表示查询到了元素，返回0 表示没有查询到元素
 */
int quicklistIndex(const quicklist *quicklist, const long long idx, quicklistEntry *entry) {
    quicklistNode *n;
    unsigned long long accum = 0;
    unsigned long long index;
    int forward = idx < 0 ? 0 : 1; /* < 0 -> reverse, 0+ -> forward */

    initEntry(entry); ///初始化entry
    entry->quicklist = quicklist; ///设置entry所属的快表

    if (!forward) { ///forward是负数，表示获取倒数第idex个元素
        index = (-idx) - 1; ///获取下标值
        n = quicklist->tail; ///获取快表的尾节点
    } else { ///如果forward是正数，表示获取第indx个元素
        index = idx;   /// 设置下标
        n = quicklist->head;    ///获取头节点
    }

    if (index >= quicklist->count) ///如果index已经超过了快表汇总的压缩表节点数量，直接返回0
        return 0;

    while (likely(n)) { ///遍历节点
        if ((accum + n->count) > index) { ///找到index在当前的节点中，跳出循环
            break;
        } else {
            D("Skipping over (%p) %u at accum %lld", (void *)n, n->count,
              accum);
            accum += n->count; ///如果index没有在当前的节点中，需要继续遍历下一个节点
            n = forward ? n->next : n->prev; ///通过方向确定是 遍历前驱还是后继节点
        }
    }
    if (!n)     ///如果n为空，直接返回0
        return 0;

    D("Found node: %p at accum %llu, idx %llu, sub+ %llu, sub- %llu", (void *)n,
      accum, index, index - accum, (-index) - 1 + accum);

    entry->node = n; ///设置entry所属的节点
    if (forward) {
        entry->offset = index - accum; ///设置entry的偏移量
    } else {
        entry->offset = (-index) - 1 + accum; ///设置偏移量 
    }

    quicklistDecompressNodeForUse(entry->node);///对节点进行临时解压缩
    entry->zi = ziplistIndex(entry->node->zl, entry->offset); ///设置entry的offset
    ziplistGet(entry->zi, &entry->value, &entry->sz, &entry->longval); ///将entry的信息都读取出来，放入快表节点中
    return 1;
}

//将尾quicklistNode节点的尾entry节点旋转到头quicklistNode节点的头部
void quicklistRotate(quicklist *quicklist) {
    if (quicklist->count <= 1) ///如果快表中只有一个entry，就直接返回
        return;
    
    unsigned char *p = ziplistIndex(quicklist->tail->zl, -1); ///获取entry的尾节点
    unsigned char *value;
    long long longval;
    unsigned int sz;
    char longstr[32] = {0};
    ziplistGet(p, &value, &sz, &longval); ///获取p所指位置的ziplist的值

    if (!value) {///如果value的字符串为空，表示这个压缩表节点数据为long long类型
        sz = ll2string(longstr, sizeof(longstr), longval); ///将long long类型的整数转化为z字符串
        value = (unsigned char *)longstr; ///并这个字符串赋值给value
    }
    ///将entry节点的信息保存到快表的头部
    quicklistPushHead(quicklist, value, sz);

    /* If quicklist has only one node, the head ziplist is also the
     * tail ziplist and PushHead() could have reallocated our single ziplist,
     * which would make our pre-existing 'p' unusable. */
    if (quicklist->len == 1) { ///如果快表只有一个节点
        p = ziplistIndex(quicklist->tail->zl, -1); ///返回entry节点的指针
    }
    /* Remove tail entry. */
    quicklistDelIndex(quicklist, quicklist->tail, &p); ///移除p指向的entry
}

/* pop from quicklist and return result in 'data' ptr.  Value of 'data'
 * is the return value of 'saver' function pointer if the data is NOT a number.
 *
 * If the quicklist element is a long long, then the return value is returned in
 * 'sval'.
 *
 * Return value of 0 means no elements available.
 * Return value of 1 means check 'data' and 'sval' for values.
 * If 'data' is set, use 'data' and 'sz'.  Otherwise, use 'sval'. */
///从快表的头节点或者尾节点中pop处一个entry（压缩表实体），并将它的value保存到data或者saver中
///返回0表示没有entry可c弹出，返回1表示正常的pop出元素
int quicklistPopCustom(quicklist *quicklist, int where, unsigned char **data,
                       unsigned int *sz, long long *sval,
                       void *(*saver)(unsigned char *data, unsigned int sz)) {
    unsigned char *p;
    unsigned char *vstr;
    unsigned int vlen;
    long long vlong;
    int pos = (where == QUICKLIST_HEAD) ? 0 : -1; ///where = 0表示头节点，= 1表示尾节点

    if (quicklist->count == 0) ///如果快表中的压缩表节点数量为0，则直接返回
        return 0;
 
    if (data) ///如果data不为空，需要将它置空
        *data = NULL;
    if (sz) ///如果sz不为空，需要将其初始化
        *sz = 0;
    if (sval)
        *sval = -123456789;

    quicklistNode *node; ///快表节点指针声明
    if (where == QUICKLIST_HEAD && quicklist->head) { ///在头节点弹出
        node = quicklist->head;
    } else if (where == QUICKLIST_TAIL && quicklist->tail) { ///在尾节点中弹出
        node = quicklist->tail;
    } else {
        return 0;
    }

    p = ziplistIndex(node->zl, pos); ///获取压缩表中的pos位置出的元素地址
    if (ziplistGet(p, &vstr, &vlen, &vlong)) { ///获取p所指的位置的值
        if (vstr) { ///如果字符串不为空，数据类型为long long类型整数
            if (data) ///如果data为空
                *data = saver(vstr, vlen);///调用特定的函数将字符串值保存到*data
            if (sz) ///如果sz不为空
                *sz = vlen; ///保存字符串的长度
        } else { ///如果字符串为空，表示为long long类型的数据
            if (data)
                *data = NULL;
            if (sval)
                *sval = vlong; ///保存整数值
        }
        quicklistDelIndex(quicklist, node, &p); ///将entry从ziplist中删除
        return 1; ///返回操作成功
    }
    return 0;
}

/* Return a malloc'd copy of data passed in */
REDIS_STATIC void *_quicklistSaver(unsigned char *data, unsigned int sz) {
    unsigned char *vstr;
    if (data) {
        vstr = zmalloc(sz);
        memcpy(vstr, data, sz);
        return vstr;
    }
    return NULL;
}

/* Default pop function
 *
 * Returns malloc'd value from quicklist */
int quicklistPop(quicklist *quicklist, int where, unsigned char **data,
                 unsigned int *sz, long long *slong) {
    unsigned char *vstr;
    unsigned int vlen;
    long long vlong;
    if (quicklist->count == 0)
        return 0;
    int ret = quicklistPopCustom(quicklist, where, &vstr, &vlen, &vlong,
                                 _quicklistSaver);
    if (data)
        *data = vstr;
    if (slong)
        *slong = vlong;
    if (sz)
        *sz = vlen;
    return ret;
}

///包装程序允许在HEAD / TAIL pop之间进行基于参数的切换
void quicklistPush(quicklist *quicklist, void *value, const size_t sz,
                   int where) {
    if (where == QUICKLIST_HEAD) {
        quicklistPushHead(quicklist, value, sz);
    } else if (where == QUICKLIST_TAIL) {
        quicklistPushTail(quicklist, value, sz);
    }
}

/* 在列表中创建或更新书签，当引用的书签被删除时，书签将自动更新到下一个节点。
 * 成功返回1（创建新书签或覆盖现有书签）。
 * 失败时返回0（已达到书签的最大支持数量）。
 * 注意：请使用简短的简单名称，以便快速查找字符串。
 * 注意：bookmakrk的创建可能会重新分配快速列表，因此输入指针可能会更改，这是调用者的责任来更新引用。
 */
int quicklistBookmarkCreate(quicklist **ql_ref, const char *name, quicklistNode *node) {
    quicklist *ql = *ql_ref;
    if (ql->bookmark_count >= QL_MAX_BM)///如果bookmark的数量已经到达了最大值，直接返回
        return 0;
    quicklistBookmark *bm = _quicklistBookmarkFindByName(ql, name); ///通过名字查询bookmark
    if (bm) { ///如果这个bm不为空
        bm->node = node; ///就让这个bm的节点指针指向这个节点，返回1
        return 1;
    }
    ql = zrealloc(ql, sizeof(quicklist) + (ql->bookmark_count+1) * sizeof(quicklistBookmark)); ///重新分配内存大小
    *ql_ref = ql;
    ql->bookmarks[ql->bookmark_count].node = node; ///修改快表书签指向的节点
    ql->bookmarks[ql->bookmark_count].name = zstrdup(name); ///修改快表书签的名字
    ql->bookmark_count++; ///快表的书签数量+1
    return 1;
}

/* Find the quicklist node referenced by a named bookmark.
 * When the bookmarked node is deleted the bookmark is updated to the next node,
 * and if that's the last node, the bookmark is deleted (so find returns NULL). */
///通过名字查询书签集合中是否存在该书签
quicklistNode *quicklistBookmarkFind(quicklist *ql, const char *name) {
    quicklistBookmark *bm = _quicklistBookmarkFindByName(ql, name);
    if (!bm) return NULL;
    return bm->node;
}

/* Delete a named bookmark.
 * returns 0 if bookmark was not found, and 1 if deleted.
 * Note that the bookmark memory is not freed yet, and is kept for future use. */
///通过名字删除书签
int quicklistBookmarkDelete(quicklist *ql, const char *name) {
    quicklistBookmark *bm = _quicklistBookmarkFindByName(ql, name);
    if (!bm)
        return 0;
    _quicklistBookmarkDelete(ql, bm);
    return 1;
}

///通过名字查询书签
quicklistBookmark *_quicklistBookmarkFindByName(quicklist *ql, const char *name) {
    unsigned i;
    for (i=0; i<ql->bookmark_count; i++) {
        if (!strcmp(ql->bookmarks[i].name, name)) {
            return &ql->bookmarks[i];
        }
    }
    return NULL;
}

///返回属于node节点的书签
quicklistBookmark *_quicklistBookmarkFindByNode(quicklist *ql, quicklistNode *node) {
    unsigned i;
    for (i=0; i<ql->bookmark_count; i++) {
        if (ql->bookmarks[i].node == node) {
            return &ql->bookmarks[i];
        }
    }
    return NULL;
}

///删除书签
void _quicklistBookmarkDelete(quicklist *ql, quicklistBookmark *bm) {
    int index = bm - ql->bookmarks;
    zfree(bm->name);
    ql->bookmark_count--;
    memmove(bm, bm+1, (ql->bookmark_count - index)* sizeof(*bm));
    /* NOTE: We do not shrink (realloc) the quicklist yet (to avoid resonance,
     * it may be re-used later (a call to realloc may NOP). */
}

///删除快表中所有的书签
void quicklistBookmarksClear(quicklist *ql) {
    while (ql->bookmark_count)
        zfree(ql->bookmarks[--ql->bookmark_count].name);
    /* NOTE: We do not shrink (realloc) the quick list. main use case for this
     * function is just before releasing the allocation. */
}

/* The rest of this file is test cases and test helpers. */
#ifdef REDIS_TEST
#include <stdint.h>
#include <sys/time.h>

#define assert(_e)                                                             \
    do {                                                                       \
        if (!(_e)) {                                                           \
            printf("\n\n=== ASSERTION FAILED ===\n");                          \
            printf("==> %s:%d '%s' is not true\n", __FILE__, __LINE__, #_e);   \
            err++;                                                             \
        }                                                                      \
    } while (0)

#define yell(str, ...) printf("ERROR! " str "\n\n", __VA_ARGS__)

#define OK printf("\tOK\n")

#define ERROR                                                                  \
    do {                                                                       \
        printf("\tERROR!\n");                                                  \
        err++;                                                                 \
    } while (0)

#define ERR(x, ...)                                                            \
    do {                                                                       \
        printf("%s:%s:%d:\t", __FILE__, __FUNCTION__, __LINE__);               \
        printf("ERROR! " x "\n", __VA_ARGS__);                                 \
        err++;                                                                 \
    } while (0)

#define TEST(name) printf("test — %s\n", name);
#define TEST_DESC(name, ...) printf("test — " name "\n", __VA_ARGS__);

#define QL_TEST_VERBOSE 0

#define UNUSED(x) (void)(x)
static void ql_info(quicklist *ql) {
#if QL_TEST_VERBOSE
    printf("Container length: %lu\n", ql->len);
    printf("Container size: %lu\n", ql->count);
    if (ql->head)
        printf("\t(zsize head: %d)\n", ziplistLen(ql->head->zl));
    if (ql->tail)
        printf("\t(zsize tail: %d)\n", ziplistLen(ql->tail->zl));
    printf("\n");
#else
    UNUSED(ql);
#endif
}

/* Return the UNIX time in microseconds */
static long long ustime(void) {
    struct timeval tv;
    long long ust;

    gettimeofday(&tv, NULL);
    ust = ((long long)tv.tv_sec) * 1000000;
    ust += tv.tv_usec;
    return ust;
}

/* Return the UNIX time in milliseconds */
static long long mstime(void) { return ustime() / 1000; }

/* Iterate over an entire quicklist.
 * Print the list if 'print' == 1.
 *
 * Returns physical count of elements found by iterating over the list. */
static int _itrprintr(quicklist *ql, int print, int forward) {
    quicklistIter *iter =
        quicklistGetIterator(ql, forward ? AL_START_HEAD : AL_START_TAIL);
    quicklistEntry entry;
    int i = 0;
    int p = 0;
    quicklistNode *prev = NULL;
    while (quicklistNext(iter, &entry)) {
        if (entry.node != prev) {
            /* Count the number of list nodes too */
            p++;
            prev = entry.node;
        }
        if (print) {
            printf("[%3d (%2d)]: [%.*s] (%lld)\n", i, p, entry.sz,
                   (char *)entry.value, entry.longval);
        }
        i++;
    }
    quicklistReleaseIterator(iter);
    return i;
}
static int itrprintr(quicklist *ql, int print) {
    return _itrprintr(ql, print, 1);
}

static int itrprintr_rev(quicklist *ql, int print) {
    return _itrprintr(ql, print, 0);
}

#define ql_verify(a, b, c, d, e)                                               \
    do {                                                                       \
        err += _ql_verify((a), (b), (c), (d), (e));                            \
    } while (0)

/* Verify list metadata matches physical list contents. */
static int _ql_verify(quicklist *ql, uint32_t len, uint32_t count,
                      uint32_t head_count, uint32_t tail_count) {
    int errors = 0;

    ql_info(ql);
    if (len != ql->len) {
        yell("quicklist length wrong: expected %d, got %u", len, ql->len);
        errors++;
    }

    if (count != ql->count) {
        yell("quicklist count wrong: expected %d, got %lu", count, ql->count);
        errors++;
    }

    int loopr = itrprintr(ql, 0);
    if (loopr != (int)ql->count) {
        yell("quicklist cached count not match actual count: expected %lu, got "
             "%d",
             ql->count, loopr);
        errors++;
    }

    int rloopr = itrprintr_rev(ql, 0);
    if (loopr != rloopr) {
        yell("quicklist has different forward count than reverse count!  "
             "Forward count is %d, reverse count is %d.",
             loopr, rloopr);
        errors++;
    }

    if (ql->len == 0 && !errors) {
        OK;
        return errors;
    }

    if (ql->head && head_count != ql->head->count &&
        head_count != ziplistLen(ql->head->zl)) {
        yell("quicklist head count wrong: expected %d, "
             "got cached %d vs. actual %d",
             head_count, ql->head->count, ziplistLen(ql->head->zl));
        errors++;
    }

    if (ql->tail && tail_count != ql->tail->count &&
        tail_count != ziplistLen(ql->tail->zl)) {
        yell("quicklist tail count wrong: expected %d, "
             "got cached %u vs. actual %d",
             tail_count, ql->tail->count, ziplistLen(ql->tail->zl));
        errors++;
    }

    if (quicklistAllowsCompression(ql)) {
        quicklistNode *node = ql->head;
        unsigned int low_raw = ql->compress;
        unsigned int high_raw = ql->len - ql->compress;

        for (unsigned int at = 0; at < ql->len; at++, node = node->next) {
            if (node && (at < low_raw || at >= high_raw)) {
                if (node->encoding != QUICKLIST_NODE_ENCODING_RAW) {
                    yell("Incorrect compression: node %d is "
                         "compressed at depth %d ((%u, %u); total "
                         "nodes: %u; size: %u; recompress: %d)",
                         at, ql->compress, low_raw, high_raw, ql->len, node->sz,
                         node->recompress);
                    errors++;
                }
            } else {
                if (node->encoding != QUICKLIST_NODE_ENCODING_LZF &&
                    !node->attempted_compress) {
                    yell("Incorrect non-compression: node %d is NOT "
                         "compressed at depth %d ((%u, %u); total "
                         "nodes: %u; size: %u; recompress: %d; attempted: %d)",
                         at, ql->compress, low_raw, high_raw, ql->len, node->sz,
                         node->recompress, node->attempted_compress);
                    errors++;
                }
            }
        }
    }

    if (!errors)
        OK;
    return errors;
}

/* Generate new string concatenating integer i against string 'prefix' */
static char *genstr(char *prefix, int i) {
    static char result[64] = {0};
    snprintf(result, sizeof(result), "%s%d", prefix, i);
    return result;
}

/* main test, but callable from other files */
int quicklistTest(int argc, char *argv[]) {
    UNUSED(argc);
    UNUSED(argv);

    unsigned int err = 0;
    int optimize_start =
        -(int)(sizeof(optimization_level) / sizeof(*optimization_level));

    printf("Starting optimization offset at: %d\n", optimize_start);

    int options[] = {0, 1, 2, 3, 4, 5, 6, 10};
    size_t option_count = sizeof(options) / sizeof(*options);
    long long runtime[option_count];

    for (int _i = 0; _i < (int)option_count; _i++) {
        printf("Testing Option %d\n", options[_i]);
        long long start = mstime();

        TEST("create list") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            ql_verify(ql, 0, 0, 0, 0);
            quicklistRelease(ql);
        }

        TEST("add to tail of empty list") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistPushTail(ql, "hello", 6);
            /* 1 for head and 1 for tail because 1 node = head = tail */
            ql_verify(ql, 1, 1, 1, 1);
            quicklistRelease(ql);
        }

        TEST("add to head of empty list") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistPushHead(ql, "hello", 6);
            /* 1 for head and 1 for tail because 1 node = head = tail */
            ql_verify(ql, 1, 1, 1, 1);
            quicklistRelease(ql);
        }

        for (int f = optimize_start; f < 32; f++) {
            TEST_DESC("add to tail 5x at fill %d at compress %d", f,
                      options[_i]) {
                quicklist *ql = quicklistNew(f, options[_i]);
                for (int i = 0; i < 5; i++)
                    quicklistPushTail(ql, genstr("hello", i), 32);
                if (ql->count != 5)
                    ERROR;
                if (f == 32)
                    ql_verify(ql, 1, 5, 5, 5);
                quicklistRelease(ql);
            }
        }

        for (int f = optimize_start; f < 32; f++) {
            TEST_DESC("add to head 5x at fill %d at compress %d", f,
                      options[_i]) {
                quicklist *ql = quicklistNew(f, options[_i]);
                for (int i = 0; i < 5; i++)
                    quicklistPushHead(ql, genstr("hello", i), 32);
                if (ql->count != 5)
                    ERROR;
                if (f == 32)
                    ql_verify(ql, 1, 5, 5, 5);
                quicklistRelease(ql);
            }
        }

        for (int f = optimize_start; f < 512; f++) {
            TEST_DESC("add to tail 500x at fill %d at compress %d", f,
                      options[_i]) {
                quicklist *ql = quicklistNew(f, options[_i]);
                for (int i = 0; i < 500; i++)
                    quicklistPushTail(ql, genstr("hello", i), 64);
                if (ql->count != 500)
                    ERROR;
                if (f == 32)
                    ql_verify(ql, 16, 500, 32, 20);
                quicklistRelease(ql);
            }
        }

        for (int f = optimize_start; f < 512; f++) {
            TEST_DESC("add to head 500x at fill %d at compress %d", f,
                      options[_i]) {
                quicklist *ql = quicklistNew(f, options[_i]);
                for (int i = 0; i < 500; i++)
                    quicklistPushHead(ql, genstr("hello", i), 32);
                if (ql->count != 500)
                    ERROR;
                if (f == 32)
                    ql_verify(ql, 16, 500, 20, 32);
                quicklistRelease(ql);
            }
        }

        TEST("rotate empty") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistRotate(ql);
            ql_verify(ql, 0, 0, 0, 0);
            quicklistRelease(ql);
        }

        for (int f = optimize_start; f < 32; f++) {
            TEST("rotate one val once") {
                quicklist *ql = quicklistNew(f, options[_i]);
                quicklistPushHead(ql, "hello", 6);
                quicklistRotate(ql);
                /* Ignore compression verify because ziplist is
                 * too small to compress. */
                ql_verify(ql, 1, 1, 1, 1);
                quicklistRelease(ql);
            }
        }

        for (int f = optimize_start; f < 3; f++) {
            TEST_DESC("rotate 500 val 5000 times at fill %d at compress %d", f,
                      options[_i]) {
                quicklist *ql = quicklistNew(f, options[_i]);
                quicklistPushHead(ql, "900", 3);
                quicklistPushHead(ql, "7000", 4);
                quicklistPushHead(ql, "-1200", 5);
                quicklistPushHead(ql, "42", 2);
                for (int i = 0; i < 500; i++)
                    quicklistPushHead(ql, genstr("hello", i), 64);
                ql_info(ql);
                for (int i = 0; i < 5000; i++) {
                    ql_info(ql);
                    quicklistRotate(ql);
                }
                if (f == 1)
                    ql_verify(ql, 504, 504, 1, 1);
                else if (f == 2)
                    ql_verify(ql, 252, 504, 2, 2);
                else if (f == 32)
                    ql_verify(ql, 16, 504, 32, 24);
                quicklistRelease(ql);
            }
        }

        TEST("pop empty") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistPop(ql, QUICKLIST_HEAD, NULL, NULL, NULL);
            ql_verify(ql, 0, 0, 0, 0);
            quicklistRelease(ql);
        }

        TEST("pop 1 string from 1") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            char *populate = genstr("hello", 331);
            quicklistPushHead(ql, populate, 32);
            unsigned char *data;
            unsigned int sz;
            long long lv;
            ql_info(ql);
            quicklistPop(ql, QUICKLIST_HEAD, &data, &sz, &lv);
            assert(data != NULL);
            assert(sz == 32);
            if (strcmp(populate, (char *)data))
                ERR("Pop'd value (%.*s) didn't equal original value (%s)", sz,
                    data, populate);
            zfree(data);
            ql_verify(ql, 0, 0, 0, 0);
            quicklistRelease(ql);
        }

        TEST("pop head 1 number from 1") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistPushHead(ql, "55513", 5);
            unsigned char *data;
            unsigned int sz;
            long long lv;
            ql_info(ql);
            quicklistPop(ql, QUICKLIST_HEAD, &data, &sz, &lv);
            assert(data == NULL);
            assert(lv == 55513);
            ql_verify(ql, 0, 0, 0, 0);
            quicklistRelease(ql);
        }

        TEST("pop head 500 from 500") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            for (int i = 0; i < 500; i++)
                quicklistPushHead(ql, genstr("hello", i), 32);
            ql_info(ql);
            for (int i = 0; i < 500; i++) {
                unsigned char *data;
                unsigned int sz;
                long long lv;
                int ret = quicklistPop(ql, QUICKLIST_HEAD, &data, &sz, &lv);
                assert(ret == 1);
                assert(data != NULL);
                assert(sz == 32);
                if (strcmp(genstr("hello", 499 - i), (char *)data))
                    ERR("Pop'd value (%.*s) didn't equal original value (%s)",
                        sz, data, genstr("hello", 499 - i));
                zfree(data);
            }
            ql_verify(ql, 0, 0, 0, 0);
            quicklistRelease(ql);
        }

        TEST("pop head 5000 from 500") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            for (int i = 0; i < 500; i++)
                quicklistPushHead(ql, genstr("hello", i), 32);
            for (int i = 0; i < 5000; i++) {
                unsigned char *data;
                unsigned int sz;
                long long lv;
                int ret = quicklistPop(ql, QUICKLIST_HEAD, &data, &sz, &lv);
                if (i < 500) {
                    assert(ret == 1);
                    assert(data != NULL);
                    assert(sz == 32);
                    if (strcmp(genstr("hello", 499 - i), (char *)data))
                        ERR("Pop'd value (%.*s) didn't equal original value "
                            "(%s)",
                            sz, data, genstr("hello", 499 - i));
                    zfree(data);
                } else {
                    assert(ret == 0);
                }
            }
            ql_verify(ql, 0, 0, 0, 0);
            quicklistRelease(ql);
        }

        TEST("iterate forward over 500 list") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistSetFill(ql, 32);
            for (int i = 0; i < 500; i++)
                quicklistPushHead(ql, genstr("hello", i), 32);
            quicklistIter *iter = quicklistGetIterator(ql, AL_START_HEAD);
            quicklistEntry entry;
            int i = 499, count = 0;
            while (quicklistNext(iter, &entry)) {
                char *h = genstr("hello", i);
                if (strcmp((char *)entry.value, h))
                    ERR("value [%s] didn't match [%s] at position %d",
                        entry.value, h, i);
                i--;
                count++;
            }
            if (count != 500)
                ERR("Didn't iterate over exactly 500 elements (%d)", i);
            ql_verify(ql, 16, 500, 20, 32);
            quicklistReleaseIterator(iter);
            quicklistRelease(ql);
        }

        TEST("iterate reverse over 500 list") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistSetFill(ql, 32);
            for (int i = 0; i < 500; i++)
                quicklistPushHead(ql, genstr("hello", i), 32);
            quicklistIter *iter = quicklistGetIterator(ql, AL_START_TAIL);
            quicklistEntry entry;
            int i = 0;
            while (quicklistNext(iter, &entry)) {
                char *h = genstr("hello", i);
                if (strcmp((char *)entry.value, h))
                    ERR("value [%s] didn't match [%s] at position %d",
                        entry.value, h, i);
                i++;
            }
            if (i != 500)
                ERR("Didn't iterate over exactly 500 elements (%d)", i);
            ql_verify(ql, 16, 500, 20, 32);
            quicklistReleaseIterator(iter);
            quicklistRelease(ql);
        }

        TEST("insert before with 0 elements") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistEntry entry;
            quicklistIndex(ql, 0, &entry);
            quicklistInsertBefore(ql, &entry, "abc", 4);
            ql_verify(ql, 1, 1, 1, 1);
            quicklistRelease(ql);
        }

        TEST("insert after with 0 elements") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistEntry entry;
            quicklistIndex(ql, 0, &entry);
            quicklistInsertAfter(ql, &entry, "abc", 4);
            ql_verify(ql, 1, 1, 1, 1);
            quicklistRelease(ql);
        }

        TEST("insert after 1 element") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistPushHead(ql, "hello", 6);
            quicklistEntry entry;
            quicklistIndex(ql, 0, &entry);
            quicklistInsertAfter(ql, &entry, "abc", 4);
            ql_verify(ql, 1, 2, 2, 2);
            quicklistRelease(ql);
        }

        TEST("insert before 1 element") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistPushHead(ql, "hello", 6);
            quicklistEntry entry;
            quicklistIndex(ql, 0, &entry);
            quicklistInsertAfter(ql, &entry, "abc", 4);
            ql_verify(ql, 1, 2, 2, 2);
            quicklistRelease(ql);
        }

        for (int f = optimize_start; f < 12; f++) {
            TEST_DESC("insert once in elements while iterating at fill %d at "
                      "compress %d\n",
                      f, options[_i]) {
                quicklist *ql = quicklistNew(f, options[_i]);
                quicklistPushTail(ql, "abc", 3);
                quicklistSetFill(ql, 1);
                quicklistPushTail(ql, "def", 3); /* force to unique node */
                quicklistSetFill(ql, f);
                quicklistPushTail(ql, "bob", 3); /* force to reset for +3 */
                quicklistPushTail(ql, "foo", 3);
                quicklistPushTail(ql, "zoo", 3);

                itrprintr(ql, 0);
                /* insert "bar" before "bob" while iterating over list. */
                quicklistIter *iter = quicklistGetIterator(ql, AL_START_HEAD);
                quicklistEntry entry;
                while (quicklistNext(iter, &entry)) {
                    if (!strncmp((char *)entry.value, "bob", 3)) {
                        /* Insert as fill = 1 so it spills into new node. */
                        quicklistInsertBefore(ql, &entry, "bar", 3);
                        break; /* didn't we fix insert-while-iterating? */
                    }
                }
                itrprintr(ql, 0);

                /* verify results */
                quicklistIndex(ql, 0, &entry);
                if (strncmp((char *)entry.value, "abc", 3))
                    ERR("Value 0 didn't match, instead got: %.*s", entry.sz,
                        entry.value);
                quicklistIndex(ql, 1, &entry);
                if (strncmp((char *)entry.value, "def", 3))
                    ERR("Value 1 didn't match, instead got: %.*s", entry.sz,
                        entry.value);
                quicklistIndex(ql, 2, &entry);
                if (strncmp((char *)entry.value, "bar", 3))
                    ERR("Value 2 didn't match, instead got: %.*s", entry.sz,
                        entry.value);
                quicklistIndex(ql, 3, &entry);
                if (strncmp((char *)entry.value, "bob", 3))
                    ERR("Value 3 didn't match, instead got: %.*s", entry.sz,
                        entry.value);
                quicklistIndex(ql, 4, &entry);
                if (strncmp((char *)entry.value, "foo", 3))
                    ERR("Value 4 didn't match, instead got: %.*s", entry.sz,
                        entry.value);
                quicklistIndex(ql, 5, &entry);
                if (strncmp((char *)entry.value, "zoo", 3))
                    ERR("Value 5 didn't match, instead got: %.*s", entry.sz,
                        entry.value);
                quicklistReleaseIterator(iter);
                quicklistRelease(ql);
            }
        }

        for (int f = optimize_start; f < 1024; f++) {
            TEST_DESC(
                "insert [before] 250 new in middle of 500 elements at fill"
                " %d at compress %d",
                f, options[_i]) {
                quicklist *ql = quicklistNew(f, options[_i]);
                for (int i = 0; i < 500; i++)
                    quicklistPushTail(ql, genstr("hello", i), 32);
                for (int i = 0; i < 250; i++) {
                    quicklistEntry entry;
                    quicklistIndex(ql, 250, &entry);
                    quicklistInsertBefore(ql, &entry, genstr("abc", i), 32);
                }
                if (f == 32)
                    ql_verify(ql, 25, 750, 32, 20);
                quicklistRelease(ql);
            }
        }

        for (int f = optimize_start; f < 1024; f++) {
            TEST_DESC("insert [after] 250 new in middle of 500 elements at "
                      "fill %d at compress %d",
                      f, options[_i]) {
                quicklist *ql = quicklistNew(f, options[_i]);
                for (int i = 0; i < 500; i++)
                    quicklistPushHead(ql, genstr("hello", i), 32);
                for (int i = 0; i < 250; i++) {
                    quicklistEntry entry;
                    quicklistIndex(ql, 250, &entry);
                    quicklistInsertAfter(ql, &entry, genstr("abc", i), 32);
                }

                if (ql->count != 750)
                    ERR("List size not 750, but rather %ld", ql->count);

                if (f == 32)
                    ql_verify(ql, 26, 750, 20, 32);
                quicklistRelease(ql);
            }
        }

        TEST("duplicate empty list") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            ql_verify(ql, 0, 0, 0, 0);
            quicklist *copy = quicklistDup(ql);
            ql_verify(copy, 0, 0, 0, 0);
            quicklistRelease(ql);
            quicklistRelease(copy);
        }

        TEST("duplicate list of 1 element") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistPushHead(ql, genstr("hello", 3), 32);
            ql_verify(ql, 1, 1, 1, 1);
            quicklist *copy = quicklistDup(ql);
            ql_verify(copy, 1, 1, 1, 1);
            quicklistRelease(ql);
            quicklistRelease(copy);
        }

        TEST("duplicate list of 500") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistSetFill(ql, 32);
            for (int i = 0; i < 500; i++)
                quicklistPushHead(ql, genstr("hello", i), 32);
            ql_verify(ql, 16, 500, 20, 32);

            quicklist *copy = quicklistDup(ql);
            ql_verify(copy, 16, 500, 20, 32);
            quicklistRelease(ql);
            quicklistRelease(copy);
        }

        for (int f = optimize_start; f < 512; f++) {
            TEST_DESC("index 1,200 from 500 list at fill %d at compress %d", f,
                      options[_i]) {
                quicklist *ql = quicklistNew(f, options[_i]);
                for (int i = 0; i < 500; i++)
                    quicklistPushTail(ql, genstr("hello", i + 1), 32);
                quicklistEntry entry;
                quicklistIndex(ql, 1, &entry);
                if (!strcmp((char *)entry.value, "hello2"))
                    OK;
                else
                    ERR("Value: %s", entry.value);
                quicklistIndex(ql, 200, &entry);
                if (!strcmp((char *)entry.value, "hello201"))
                    OK;
                else
                    ERR("Value: %s", entry.value);
                quicklistRelease(ql);
            }

            TEST_DESC("index -1,-2 from 500 list at fill %d at compress %d", f,
                      options[_i]) {
                quicklist *ql = quicklistNew(f, options[_i]);
                for (int i = 0; i < 500; i++)
                    quicklistPushTail(ql, genstr("hello", i + 1), 32);
                quicklistEntry entry;
                quicklistIndex(ql, -1, &entry);
                if (!strcmp((char *)entry.value, "hello500"))
                    OK;
                else
                    ERR("Value: %s", entry.value);
                quicklistIndex(ql, -2, &entry);
                if (!strcmp((char *)entry.value, "hello499"))
                    OK;
                else
                    ERR("Value: %s", entry.value);
                quicklistRelease(ql);
            }

            TEST_DESC("index -100 from 500 list at fill %d at compress %d", f,
                      options[_i]) {
                quicklist *ql = quicklistNew(f, options[_i]);
                for (int i = 0; i < 500; i++)
                    quicklistPushTail(ql, genstr("hello", i + 1), 32);
                quicklistEntry entry;
                quicklistIndex(ql, -100, &entry);
                if (!strcmp((char *)entry.value, "hello401"))
                    OK;
                else
                    ERR("Value: %s", entry.value);
                quicklistRelease(ql);
            }

            TEST_DESC("index too big +1 from 50 list at fill %d at compress %d",
                      f, options[_i]) {
                quicklist *ql = quicklistNew(f, options[_i]);
                for (int i = 0; i < 50; i++)
                    quicklistPushTail(ql, genstr("hello", i + 1), 32);
                quicklistEntry entry;
                if (quicklistIndex(ql, 50, &entry))
                    ERR("Index found at 50 with 50 list: %.*s", entry.sz,
                        entry.value);
                else
                    OK;
                quicklistRelease(ql);
            }
        }

        TEST("delete range empty list") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistDelRange(ql, 5, 20);
            ql_verify(ql, 0, 0, 0, 0);
            quicklistRelease(ql);
        }

        TEST("delete range of entire node in list of one node") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            for (int i = 0; i < 32; i++)
                quicklistPushHead(ql, genstr("hello", i), 32);
            ql_verify(ql, 1, 32, 32, 32);
            quicklistDelRange(ql, 0, 32);
            ql_verify(ql, 0, 0, 0, 0);
            quicklistRelease(ql);
        }

        TEST("delete range of entire node with overflow counts") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            for (int i = 0; i < 32; i++)
                quicklistPushHead(ql, genstr("hello", i), 32);
            ql_verify(ql, 1, 32, 32, 32);
            quicklistDelRange(ql, 0, 128);
            ql_verify(ql, 0, 0, 0, 0);
            quicklistRelease(ql);
        }

        TEST("delete middle 100 of 500 list") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistSetFill(ql, 32);
            for (int i = 0; i < 500; i++)
                quicklistPushTail(ql, genstr("hello", i + 1), 32);
            ql_verify(ql, 16, 500, 32, 20);
            quicklistDelRange(ql, 200, 100);
            ql_verify(ql, 14, 400, 32, 20);
            quicklistRelease(ql);
        }

        TEST("delete negative 1 from 500 list") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistSetFill(ql, 32);
            for (int i = 0; i < 500; i++)
                quicklistPushTail(ql, genstr("hello", i + 1), 32);
            ql_verify(ql, 16, 500, 32, 20);
            quicklistDelRange(ql, -1, 1);
            ql_verify(ql, 16, 499, 32, 19);
            quicklistRelease(ql);
        }

        TEST("delete negative 1 from 500 list with overflow counts") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistSetFill(ql, 32);
            for (int i = 0; i < 500; i++)
                quicklistPushTail(ql, genstr("hello", i + 1), 32);
            ql_verify(ql, 16, 500, 32, 20);
            quicklistDelRange(ql, -1, 128);
            ql_verify(ql, 16, 499, 32, 19);
            quicklistRelease(ql);
        }

        TEST("delete negative 100 from 500 list") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistSetFill(ql, 32);
            for (int i = 0; i < 500; i++)
                quicklistPushTail(ql, genstr("hello", i + 1), 32);
            quicklistDelRange(ql, -100, 100);
            ql_verify(ql, 13, 400, 32, 16);
            quicklistRelease(ql);
        }

        TEST("delete -10 count 5 from 50 list") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistSetFill(ql, 32);
            for (int i = 0; i < 50; i++)
                quicklistPushTail(ql, genstr("hello", i + 1), 32);
            ql_verify(ql, 2, 50, 32, 18);
            quicklistDelRange(ql, -10, 5);
            ql_verify(ql, 2, 45, 32, 13);
            quicklistRelease(ql);
        }

        TEST("numbers only list read") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistPushTail(ql, "1111", 4);
            quicklistPushTail(ql, "2222", 4);
            quicklistPushTail(ql, "3333", 4);
            quicklistPushTail(ql, "4444", 4);
            ql_verify(ql, 1, 4, 4, 4);
            quicklistEntry entry;
            quicklistIndex(ql, 0, &entry);
            if (entry.longval != 1111)
                ERR("Not 1111, %lld", entry.longval);
            quicklistIndex(ql, 1, &entry);
            if (entry.longval != 2222)
                ERR("Not 2222, %lld", entry.longval);
            quicklistIndex(ql, 2, &entry);
            if (entry.longval != 3333)
                ERR("Not 3333, %lld", entry.longval);
            quicklistIndex(ql, 3, &entry);
            if (entry.longval != 4444)
                ERR("Not 4444, %lld", entry.longval);
            if (quicklistIndex(ql, 4, &entry))
                ERR("Index past elements: %lld", entry.longval);
            quicklistIndex(ql, -1, &entry);
            if (entry.longval != 4444)
                ERR("Not 4444 (reverse), %lld", entry.longval);
            quicklistIndex(ql, -2, &entry);
            if (entry.longval != 3333)
                ERR("Not 3333 (reverse), %lld", entry.longval);
            quicklistIndex(ql, -3, &entry);
            if (entry.longval != 2222)
                ERR("Not 2222 (reverse), %lld", entry.longval);
            quicklistIndex(ql, -4, &entry);
            if (entry.longval != 1111)
                ERR("Not 1111 (reverse), %lld", entry.longval);
            if (quicklistIndex(ql, -5, &entry))
                ERR("Index past elements (reverse), %lld", entry.longval);
            quicklistRelease(ql);
        }

        TEST("numbers larger list read") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistSetFill(ql, 32);
            char num[32];
            long long nums[5000];
            for (int i = 0; i < 5000; i++) {
                nums[i] = -5157318210846258176 + i;
                int sz = ll2string(num, sizeof(num), nums[i]);
                quicklistPushTail(ql, num, sz);
            }
            quicklistPushTail(ql, "xxxxxxxxxxxxxxxxxxxx", 20);
            quicklistEntry entry;
            for (int i = 0; i < 5000; i++) {
                quicklistIndex(ql, i, &entry);
                if (entry.longval != nums[i])
                    ERR("[%d] Not longval %lld but rather %lld", i, nums[i],
                        entry.longval);
                entry.longval = 0xdeadbeef;
            }
            quicklistIndex(ql, 5000, &entry);
            if (strncmp((char *)entry.value, "xxxxxxxxxxxxxxxxxxxx", 20))
                ERR("String val not match: %s", entry.value);
            ql_verify(ql, 157, 5001, 32, 9);
            quicklistRelease(ql);
        }

        TEST("numbers larger list read B") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistPushTail(ql, "99", 2);
            quicklistPushTail(ql, "98", 2);
            quicklistPushTail(ql, "xxxxxxxxxxxxxxxxxxxx", 20);
            quicklistPushTail(ql, "96", 2);
            quicklistPushTail(ql, "95", 2);
            quicklistReplaceAtIndex(ql, 1, "foo", 3);
            quicklistReplaceAtIndex(ql, -1, "bar", 3);
            quicklistRelease(ql);
            OK;
        }

        for (int f = optimize_start; f < 16; f++) {
            TEST_DESC("lrem test at fill %d at compress %d", f, options[_i]) {
                quicklist *ql = quicklistNew(f, options[_i]);
                char *words[] = {"abc", "foo", "bar",  "foobar", "foobared",
                                 "zap", "bar", "test", "foo"};
                char *result[] = {"abc", "foo",  "foobar", "foobared",
                                  "zap", "test", "foo"};
                char *resultB[] = {"abc",      "foo", "foobar",
                                   "foobared", "zap", "test"};
                for (int i = 0; i < 9; i++)
                    quicklistPushTail(ql, words[i], strlen(words[i]));

                /* lrem 0 bar */
                quicklistIter *iter = quicklistGetIterator(ql, AL_START_HEAD);
                quicklistEntry entry;
                int i = 0;
                while (quicklistNext(iter, &entry)) {
                    if (quicklistCompare(entry.zi, (unsigned char *)"bar", 3)) {
                        quicklistDelEntry(iter, &entry);
                    }
                    i++;
                }
                quicklistReleaseIterator(iter);

                /* check result of lrem 0 bar */
                iter = quicklistGetIterator(ql, AL_START_HEAD);
                i = 0;
                int ok = 1;
                while (quicklistNext(iter, &entry)) {
                    /* Result must be: abc, foo, foobar, foobared, zap, test,
                     * foo */
                    if (strncmp((char *)entry.value, result[i], entry.sz)) {
                        ERR("No match at position %d, got %.*s instead of %s",
                            i, entry.sz, entry.value, result[i]);
                        ok = 0;
                    }
                    i++;
                }
                quicklistReleaseIterator(iter);

                quicklistPushTail(ql, "foo", 3);

                /* lrem -2 foo */
                iter = quicklistGetIterator(ql, AL_START_TAIL);
                i = 0;
                int del = 2;
                while (quicklistNext(iter, &entry)) {
                    if (quicklistCompare(entry.zi, (unsigned char *)"foo", 3)) {
                        quicklistDelEntry(iter, &entry);
                        del--;
                    }
                    if (!del)
                        break;
                    i++;
                }
                quicklistReleaseIterator(iter);

                /* check result of lrem -2 foo */
                /* (we're ignoring the '2' part and still deleting all foo
                 * because
                 * we only have two foo) */
                iter = quicklistGetIterator(ql, AL_START_TAIL);
                i = 0;
                size_t resB = sizeof(resultB) / sizeof(*resultB);
                while (quicklistNext(iter, &entry)) {
                    /* Result must be: abc, foo, foobar, foobared, zap, test,
                     * foo */
                    if (strncmp((char *)entry.value, resultB[resB - 1 - i],
                                entry.sz)) {
                        ERR("No match at position %d, got %.*s instead of %s",
                            i, entry.sz, entry.value, resultB[resB - 1 - i]);
                        ok = 0;
                    }
                    i++;
                }

                quicklistReleaseIterator(iter);
                /* final result of all tests */
                if (ok)
                    OK;
                quicklistRelease(ql);
            }
        }

        for (int f = optimize_start; f < 16; f++) {
            TEST_DESC("iterate reverse + delete at fill %d at compress %d", f,
                      options[_i]) {
                quicklist *ql = quicklistNew(f, options[_i]);
                quicklistPushTail(ql, "abc", 3);
                quicklistPushTail(ql, "def", 3);
                quicklistPushTail(ql, "hij", 3);
                quicklistPushTail(ql, "jkl", 3);
                quicklistPushTail(ql, "oop", 3);

                quicklistEntry entry;
                quicklistIter *iter = quicklistGetIterator(ql, AL_START_TAIL);
                int i = 0;
                while (quicklistNext(iter, &entry)) {
                    if (quicklistCompare(entry.zi, (unsigned char *)"hij", 3)) {
                        quicklistDelEntry(iter, &entry);
                    }
                    i++;
                }
                quicklistReleaseIterator(iter);

                if (i != 5)
                    ERR("Didn't iterate 5 times, iterated %d times.", i);

                /* Check results after deletion of "hij" */
                iter = quicklistGetIterator(ql, AL_START_HEAD);
                i = 0;
                char *vals[] = {"abc", "def", "jkl", "oop"};
                while (quicklistNext(iter, &entry)) {
                    if (!quicklistCompare(entry.zi, (unsigned char *)vals[i],
                                          3)) {
                        ERR("Value at %d didn't match %s\n", i, vals[i]);
                    }
                    i++;
                }
                quicklistReleaseIterator(iter);
                quicklistRelease(ql);
            }
        }

        for (int f = optimize_start; f < 800; f++) {
            TEST_DESC("iterator at index test at fill %d at compress %d", f,
                      options[_i]) {
                quicklist *ql = quicklistNew(f, options[_i]);
                char num[32];
                long long nums[5000];
                for (int i = 0; i < 760; i++) {
                    nums[i] = -5157318210846258176 + i;
                    int sz = ll2string(num, sizeof(num), nums[i]);
                    quicklistPushTail(ql, num, sz);
                }

                quicklistEntry entry;
                quicklistIter *iter =
                    quicklistGetIteratorAtIdx(ql, AL_START_HEAD, 437);
                int i = 437;
                while (quicklistNext(iter, &entry)) {
                    if (entry.longval != nums[i])
                        ERR("Expected %lld, but got %lld", entry.longval,
                            nums[i]);
                    i++;
                }
                quicklistReleaseIterator(iter);
                quicklistRelease(ql);
            }
        }

        for (int f = optimize_start; f < 40; f++) {
            TEST_DESC("ltrim test A at fill %d at compress %d", f,
                      options[_i]) {
                quicklist *ql = quicklistNew(f, options[_i]);
                char num[32];
                long long nums[5000];
                for (int i = 0; i < 32; i++) {
                    nums[i] = -5157318210846258176 + i;
                    int sz = ll2string(num, sizeof(num), nums[i]);
                    quicklistPushTail(ql, num, sz);
                }
                if (f == 32)
                    ql_verify(ql, 1, 32, 32, 32);
                /* ltrim 25 53 (keep [25,32] inclusive = 7 remaining) */
                quicklistDelRange(ql, 0, 25);
                quicklistDelRange(ql, 0, 0);
                quicklistEntry entry;
                for (int i = 0; i < 7; i++) {
                    quicklistIndex(ql, i, &entry);
                    if (entry.longval != nums[25 + i])
                        ERR("Deleted invalid range!  Expected %lld but got "
                            "%lld",
                            entry.longval, nums[25 + i]);
                }
                if (f == 32)
                    ql_verify(ql, 1, 7, 7, 7);
                quicklistRelease(ql);
            }
        }

        for (int f = optimize_start; f < 40; f++) {
            TEST_DESC("ltrim test B at fill %d at compress %d", f,
                      options[_i]) {
                /* Force-disable compression because our 33 sequential
                 * integers don't compress and the check always fails. */
                quicklist *ql = quicklistNew(f, QUICKLIST_NOCOMPRESS);
                char num[32];
                long long nums[5000];
                for (int i = 0; i < 33; i++) {
                    nums[i] = i;
                    int sz = ll2string(num, sizeof(num), nums[i]);
                    quicklistPushTail(ql, num, sz);
                }
                if (f == 32)
                    ql_verify(ql, 2, 33, 32, 1);
                /* ltrim 5 16 (keep [5,16] inclusive = 12 remaining) */
                quicklistDelRange(ql, 0, 5);
                quicklistDelRange(ql, -16, 16);
                if (f == 32)
                    ql_verify(ql, 1, 12, 12, 12);
                quicklistEntry entry;
                quicklistIndex(ql, 0, &entry);
                if (entry.longval != 5)
                    ERR("A: longval not 5, but %lld", entry.longval);
                else
                    OK;
                quicklistIndex(ql, -1, &entry);
                if (entry.longval != 16)
                    ERR("B! got instead: %lld", entry.longval);
                else
                    OK;
                quicklistPushTail(ql, "bobobob", 7);
                quicklistIndex(ql, -1, &entry);
                if (strncmp((char *)entry.value, "bobobob", 7))
                    ERR("Tail doesn't match bobobob, it's %.*s instead",
                        entry.sz, entry.value);
                for (int i = 0; i < 12; i++) {
                    quicklistIndex(ql, i, &entry);
                    if (entry.longval != nums[5 + i])
                        ERR("Deleted invalid range!  Expected %lld but got "
                            "%lld",
                            entry.longval, nums[5 + i]);
                }
                quicklistRelease(ql);
            }
        }

        for (int f = optimize_start; f < 40; f++) {
            TEST_DESC("ltrim test C at fill %d at compress %d", f,
                      options[_i]) {
                quicklist *ql = quicklistNew(f, options[_i]);
                char num[32];
                long long nums[5000];
                for (int i = 0; i < 33; i++) {
                    nums[i] = -5157318210846258176 + i;
                    int sz = ll2string(num, sizeof(num), nums[i]);
                    quicklistPushTail(ql, num, sz);
                }
                if (f == 32)
                    ql_verify(ql, 2, 33, 32, 1);
                /* ltrim 3 3 (keep [3,3] inclusive = 1 remaining) */
                quicklistDelRange(ql, 0, 3);
                quicklistDelRange(ql, -29,
                                  4000); /* make sure not loop forever */
                if (f == 32)
                    ql_verify(ql, 1, 1, 1, 1);
                quicklistEntry entry;
                quicklistIndex(ql, 0, &entry);
                if (entry.longval != -5157318210846258173)
                    ERROR;
                else
                    OK;
                quicklistRelease(ql);
            }
        }

        for (int f = optimize_start; f < 40; f++) {
            TEST_DESC("ltrim test D at fill %d at compress %d", f,
                      options[_i]) {
                quicklist *ql = quicklistNew(f, options[_i]);
                char num[32];
                long long nums[5000];
                for (int i = 0; i < 33; i++) {
                    nums[i] = -5157318210846258176 + i;
                    int sz = ll2string(num, sizeof(num), nums[i]);
                    quicklistPushTail(ql, num, sz);
                }
                if (f == 32)
                    ql_verify(ql, 2, 33, 32, 1);
                quicklistDelRange(ql, -12, 3);
                if (ql->count != 30)
                    ERR("Didn't delete exactly three elements!  Count is: %lu",
                        ql->count);
                quicklistRelease(ql);
            }
        }

        for (int f = optimize_start; f < 72; f++) {
            TEST_DESC("create quicklist from ziplist at fill %d at compress %d",
                      f, options[_i]) {
                unsigned char *zl = ziplistNew();
                long long nums[64];
                char num[64];
                for (int i = 0; i < 33; i++) {
                    nums[i] = -5157318210846258176 + i;
                    int sz = ll2string(num, sizeof(num), nums[i]);
                    zl =
                        ziplistPush(zl, (unsigned char *)num, sz, ZIPLIST_TAIL);
                }
                for (int i = 0; i < 33; i++) {
                    zl = ziplistPush(zl, (unsigned char *)genstr("hello", i),
                                     32, ZIPLIST_TAIL);
                }
                quicklist *ql = quicklistCreateFromZiplist(f, options[_i], zl);
                if (f == 1)
                    ql_verify(ql, 66, 66, 1, 1);
                else if (f == 32)
                    ql_verify(ql, 3, 66, 32, 2);
                else if (f == 66)
                    ql_verify(ql, 1, 66, 66, 66);
                quicklistRelease(ql);
            }
        }

        long long stop = mstime();
        runtime[_i] = stop - start;
    }

    /* Run a longer test of compression depth outside of primary test loop. */
    int list_sizes[] = {250, 251, 500, 999, 1000};
    long long start = mstime();
    for (int list = 0; list < (int)(sizeof(list_sizes) / sizeof(*list_sizes));
         list++) {
        for (int f = optimize_start; f < 128; f++) {
            for (int depth = 1; depth < 40; depth++) {
                /* skip over many redundant test cases */
                TEST_DESC("verify specific compression of interior nodes with "
                          "%d list "
                          "at fill %d at compress %d",
                          list_sizes[list], f, depth) {
                    quicklist *ql = quicklistNew(f, depth);
                    for (int i = 0; i < list_sizes[list]; i++) {
                        quicklistPushTail(ql, genstr("hello TAIL", i + 1), 64);
                        quicklistPushHead(ql, genstr("hello HEAD", i + 1), 64);
                    }

                    quicklistNode *node = ql->head;
                    unsigned int low_raw = ql->compress;
                    unsigned int high_raw = ql->len - ql->compress;

                    for (unsigned int at = 0; at < ql->len;
                         at++, node = node->next) {
                        if (at < low_raw || at >= high_raw) {
                            if (node->encoding != QUICKLIST_NODE_ENCODING_RAW) {
                                ERR("Incorrect compression: node %d is "
                                    "compressed at depth %d ((%u, %u); total "
                                    "nodes: %u; size: %u)",
                                    at, depth, low_raw, high_raw, ql->len,
                                    node->sz);
                            }
                        } else {
                            if (node->encoding != QUICKLIST_NODE_ENCODING_LZF) {
                                ERR("Incorrect non-compression: node %d is NOT "
                                    "compressed at depth %d ((%u, %u); total "
                                    "nodes: %u; size: %u; attempted: %d)",
                                    at, depth, low_raw, high_raw, ql->len,
                                    node->sz, node->attempted_compress);
                            }
                        }
                    }
                    quicklistRelease(ql);
                }
            }
        }
    }
    long long stop = mstime();

    printf("\n");
    for (size_t i = 0; i < option_count; i++)
        printf("Test Loop %02d: %0.2f seconds.\n", options[i],
               (float)runtime[i] / 1000);
    printf("Compressions: %0.2f seconds.\n", (float)(stop - start) / 1000);
    printf("\n");

    TEST("bookmark get updated to next item") {
        quicklist *ql = quicklistNew(1, 0);
        quicklistPushTail(ql, "1", 1);
        quicklistPushTail(ql, "2", 1);
        quicklistPushTail(ql, "3", 1);
        quicklistPushTail(ql, "4", 1);
        quicklistPushTail(ql, "5", 1);
        assert(ql->len==5);
        /* add two bookmarks, one pointing to the node before the last. */
        assert(quicklistBookmarkCreate(&ql, "_dummy", ql->head->next));
        assert(quicklistBookmarkCreate(&ql, "_test", ql->tail->prev));
        /* test that the bookmark returns the right node, delete it and see that the bookmark points to the last node */
        assert(quicklistBookmarkFind(ql, "_test") == ql->tail->prev);
        assert(quicklistDelRange(ql, -2, 1));
        assert(quicklistBookmarkFind(ql, "_test") == ql->tail);
        /* delete the last node, and see that the bookmark was deleted. */
        assert(quicklistDelRange(ql, -1, 1));
        assert(quicklistBookmarkFind(ql, "_test") == NULL);
        /* test that other bookmarks aren't affected */
        assert(quicklistBookmarkFind(ql, "_dummy") == ql->head->next);
        assert(quicklistBookmarkFind(ql, "_missing") == NULL);
        assert(ql->len==3);
        quicklistBookmarksClear(ql); /* for coverage */
        assert(quicklistBookmarkFind(ql, "_dummy") == NULL);
        quicklistRelease(ql);
    }

    TEST("bookmark limit") {
        int i;
        quicklist *ql = quicklistNew(1, 0);
        quicklistPushHead(ql, "1", 1);
        for (i=0; i<QL_MAX_BM; i++)
            assert(quicklistBookmarkCreate(&ql, genstr("",i), ql->head));
        /* when all bookmarks are used, creation fails */
        assert(!quicklistBookmarkCreate(&ql, "_test", ql->head));
        /* delete one and see that we can now create another */
        assert(quicklistBookmarkDelete(ql, "0"));
        assert(quicklistBookmarkCreate(&ql, "_test", ql->head));
        /* delete one and see that the rest survive */
        assert(quicklistBookmarkDelete(ql, "_test"));
        for (i=1; i<QL_MAX_BM; i++)
            assert(quicklistBookmarkFind(ql, genstr("",i)) == ql->head);
        /* make sure the deleted ones are indeed gone */
        assert(!quicklistBookmarkFind(ql, "0"));
        assert(!quicklistBookmarkFind(ql, "_test"));
        quicklistRelease(ql);
    }

    if (!err)
        printf("ALL TESTS PASSED!\n");
    else
        ERR("Sorry, not all tests passed!  In fact, %d tests failed.", err);

    return err;
}
#endif
