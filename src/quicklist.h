///quicklist 是一个通用的双向链表 
#include <stdint.h>

#ifndef __QUICKLIST_H__
#define __QUICKLIST_H__

///node，quicklist和iterator是当前唯一使用的数据结构。 

/* quicklistNode是一个32字节的结构，快表的ziplist的节点。 我们使用位字段将quicklistNode保持为32个字节。
 * count: 16 bits, max 65536 (max zl bytes is 65k, so max count actually < 32k).
 * encoding: 2 bits, RAW=1, LZF=2.
 * container: 2 bits, NONE=1, ZIPLIST=2.
 * recompress: 1 bit, bool, true 如果节点被临时压缩以供使用。
 * attempted_compress: 1 bit, boolean, 用于测试期间的验证.
 * extra: 12 bits, free for future use; pads out the remainder of 32 bits
 */
///快表中节点的数据结构定义
typedef struct quicklistNode {
    struct quicklistNode *prev; ///指向前驱节点的指针
    struct quicklistNode *next; ///指向后继节点的指针
    unsigned char *zl;          ///如果说没有设置压缩的参数，zl指向一个压缩表，如果设置了压缩参数，就指向quicklistLZF结构
    unsigned int sz;            ///压缩表的大小
    unsigned int count : 16;    ///压缩表中的节点数，用16位来表示；注意 ： 在这里表示占位符的意思
    unsigned int encoding : 2;  ///编码格式，表示是否采用LZF压缩算法进行快表节点压缩，1表示不用，2表示采用，用2位来表示
    unsigned int container : 2; ///表示一个快表节点是否采用压缩表来保存数据，1表示不采用，2表示采用。用2位来表示
    unsigned int recompress : 1;///用来标识该节点是否被压缩过，占用1位。如果recompress = 1,表示该节点等待被再次压缩
    unsigned int attempted_compress : 1; ///测试使用，如果节点太小，，就不能被压缩
    unsigned int extra : 10;    ///额外的空间，以供以后使用
} quicklistNode;

/* quicklistLZF is a 4+N byte struct holding 'sz' followed by 'compressed'.
 * 'sz' is byte length of 'compressed' field.
 * 'compressed' is LZF data with total (compressed) length 'sz'
 * 注意：未压缩的长度存储在quicklistNode-> sz中。
 * 压缩quicklistNode-> zl时，node-> zl指向quicklistLZF
 */
typedef struct quicklistLZF {
    unsigned int sz;  ///通过LZF算法压缩后压缩表的大小
    char compressed[]; ///保存被压缩后的压缩表，它是柔性数组，大小不确定
} quicklistLZF;

/* Bookmarks are padded with realloc at the end of of the quicklist struct.
 * They should only be used for very big lists if thousands of nodes were the
 * excess memory usage is negligible, and there's a real need to iterate on them
 * in portions.
 * When not used, they don't add any memory overhead, but when used and then
 * deleted, some overhead remains (to avoid resonance).
 * The number of bookmarks used should be kept to minimum since it also adds
 * overhead on node deletion (searching for a bookmark to update). */
///书签，加速查询
typedef struct quicklistBookmark {
    quicklistNode *node; ///书签所属的快表节点
    char *name; ///书签的名称
} quicklistBookmark;

#if UINTPTR_MAX == 0xffffffff
/* 32-bit */
#   define QL_FILL_BITS 14
#   define QL_COMP_BITS 14
#   define QL_BM_BITS 4
#elif UINTPTR_MAX == 0xffffffffffffffff
/* 64-bit */
#   define QL_FILL_BITS 16
#   define QL_COMP_BITS 16
#   define QL_BM_BITS 4 /* we can encode more, but we rather limit the user
                           since they cause performance degradation. */
#else
#   error unknown arch bits count
#endif

/* quicklist is a 40 byte struct (on 64-bit systems) describing a quicklist.
 * 'count' is the number of total entries.
 * 'len' is the number of quicklist nodes.
 * 'compress' is: -1 if compression disabled, otherwise it's the number
 *                of quicklistNodes to leave uncompressed at ends of quicklist.
 * 'fill' is the user-requested (or default) fill factor.
 * 'bookmakrs are an optional feature that is used by realloc this struct,
 *      so that they don't consume memory when not used. */
///快表的数据结构声明
typedef struct quicklist {
    quicklistNode *head;     ///双向链表的头部节点(指向最双向链表最左边)
    quicklistNode *tail;     ///双向链表的尾部节点（指向双向链表最右边）
    unsigned long count;     ///快表中所有压缩表节点和     
    unsigned long len;       ///快表中的节点个数     
    int fill : QL_FILL_BITS; ///保存压缩表的大小           
    unsigned int compress : QL_COMP_BITS;///保存压缩的程度，0表示不保存 
    unsigned int bookmark_count: QL_BM_BITS; ///保存bookmark的数量 
    quicklistBookmark bookmarks[]; ///保存所有bookmark的数组
} quicklist;

///快表遍历的迭代器 
typedef struct quicklistIter {
    const quicklist *quicklist; ///指向要遍历的快表
    quicklistNode *current;     ///指向遍历快表中遍历的节点
    unsigned char *zi;          ///指向节点中的ziplist迭代器
    long offset;                ///当前压缩表中的偏移量
    int direction;              ///迭代的方向
} quicklistIter;

///用于记录快表中快表节点的压缩表信息的结构体
typedef struct quicklistEntry {
    const quicklist *quicklist; ///当前所在的快表
    quicklistNode *node;        ///快表中的具体节点
    unsigned char *zi;          ///节点中压缩表的指针
    unsigned char *value;       ///压缩表中当前节点为字符串的value
    long long longval;          ///压缩表中当前节点为整数的value
    unsigned int sz;            ///压缩表的大小
    int offset;                 ///当前压缩表位置的偏移量
} quicklistEntry;

#define QUICKLIST_HEAD 0 ///从头到尾的方式遍历
#define QUICKLIST_TAIL -1 ///从未到头的方式遍历
 
/* quicklist node encodings */
#define QUICKLIST_NODE_ENCODING_RAW 1 ///没有进行压缩操作
#define QUICKLIST_NODE_ENCODING_LZF 2 ///采用LZF算法进行压缩操作

/* quicklist compression disable */
#define QUICKLIST_NOCOMPRESS 0 ///不能就快表进行压缩

/* quicklist container formats */
#define QUICKLIST_NODE_CONTAINER_NONE 1 ///快表节点中直接保存对象
#define QUICKLIST_NODE_CONTAINER_ZIPLIST 2 ///快表节点中直接保存ziplist对象

///检测压缩表是否被压缩，1表是被压缩，0表示不被压缩
#define quicklistNodeIsCompressed(node)                                        \
    ((node)->encoding == QUICKLIST_NODE_ENCODING_LZF)

quicklist *quicklistCreate(void); ///创建一个空的压缩表
quicklist *quicklistNew(int fill, int compress); ///创建一个新的快表
void quicklistSetCompressDepth(quicklist *quicklist, int depth) ; ///设置快表的a压缩深度
void quicklistSetFill(quicklist *quicklist, int fill); ///设置快表的fill参数
void quicklistSetOptions(quicklist *quicklist, int fill, int depth); ///设置快表的参数
void quicklistRelease(quicklist *quicklist); ///示范整个快表
int quicklistPushHead(quicklist *quicklist, void *value, const size_t sz); ///在快表的头部节点中加入entry
int quicklistPushTail(quicklist *quicklist, void *value, const size_t sz); ///在快表尾部节点中加入entry
void quicklistPush(quicklist *quicklist, void *value, const size_t sz,
                   int where); ///在快表节点中加入entry，通过where确定是是头节点还是尾节点插入
void quicklistAppendZiplist(quicklist *quicklist, unsigned char *zl); ///在快表的节点中追加ziplist
quicklist *quicklistAppendValuesFromZiplist(quicklist *quicklist,
                                            unsigned char *zl); ///
quicklist *quicklistCreateFromZiplist(int fill, int compress,
                                      unsigned char *zl); ///通过zl创建快表节点
void quicklistInsertAfter(quicklist *quicklist, quicklistEntry *node,
                          void *value, const size_t sz); ///在node节点后面插入entry
void quicklistInsertBefore(quicklist *quicklist, quicklistEntry *node,
                           void *value, const size_t sz); ///在node前面插入entry
void quicklistDelEntry(quicklistIter *iter, quicklistEntry *entry); ///通过迭代器的形式删除快表节点中俄entry
int quicklistReplaceAtIndex(quicklist *quicklist, long index, void *data,
                            int sz); ///替换快表节点中zl index出的数据
int quicklistDelRange(quicklist *quicklist, const long start, const long stop); ///删除快表节点中固定范围的压缩表节点
quicklistIter *quicklistGetIterator(const quicklist *quicklist, int direction);///创建一个快表迭代器
quicklistIter *quicklistGetIteratorAtIdx(const quicklist *quicklist,
                                         int direction, const long long idx); ///获取idx处的迭代器
int quicklistNext(quicklistIter *iter, quicklistEntry *node); ///通过迭代器，获取下一个节点
void quicklistReleaseIterator(quicklistIter *iter); ///释放快表迭代器
quicklist *quicklistDup(quicklist *orig); ///快表复制
int quicklistIndex(const quicklist *quicklist, const long long index,
                   quicklistEntry *entry); ///获取快表节点的压缩表index处的entry
void quicklistRewind(quicklist *quicklist, quicklistIter *li); ///
void quicklistRewindTail(quicklist *quicklist, quicklistIter *li); //
void quicklistRotate(quicklist *quicklist); ///旋转快表
int quicklistPopCustom(quicklist *quicklist, int where, unsigned char **data,
                       unsigned int *sz, long long *sval,
                       void *(*saver)(unsigned char *data, unsigned int sz));
int quicklistPop(quicklist *quicklist, int where, unsigned char **data,
                 unsigned int *sz, long long *slong); ///在头节点或者尾节点中弹出entry
unsigned long quicklistCount(const quicklist *ql);
int quicklistCompare(unsigned char *p1, unsigned char *p2, int p2_len); //比较快表节点中两个压缩表节点
size_t quicklistGetLzf(const quicklistNode *node, void **data);

/* bookmarks */
int quicklistBookmarkCreate(quicklist **ql_ref, const char *name, quicklistNode *node);
int quicklistBookmarkDelete(quicklist *ql, const char *name);
quicklistNode *quicklistBookmarkFind(quicklist *ql, const char *name);
void quicklistBookmarksClear(quicklist *ql);

#ifdef REDIS_TEST
int quicklistTest(int argc, char *argv[]);
#endif

/* Directions for iterators */
#define AL_START_HEAD 0
#define AL_START_TAIL 1

#endif /* __QUICKLIST_H__ */
