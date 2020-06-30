/*
 * 这是redis中双向链表的操作定义文件，它的实现在adlist.c文件中
 */
#ifndef __ADLIST_H__
#define __ADLIST_H__

///链表节点结构体的定义
typedef struct listNode {
    struct listNode *prev; ///指向前一个节点的指针
    struct listNode *next; ///指向下一个节点的指针
    void *value; ///当前节点的值
} listNode;

///链表遍历的iterator结构体的定义
typedef struct listIter {
    listNode *next; ///获取遍历的节点，也就是当前节点，这里并非是next节点，不要被名字迷惑了
    int direction; ///遍历的方向，如果是0（宏定义中的AL_START_HEAD）表示从头向尾遍历，如果是1（AL_START_TAIL），表示从尾到头遍历
} listIter;

///链表结构体的定义
typedef struct list {
    listNode *head; ///链表的头节点
    listNode *tail; ///链表的尾节点
    void *(*dup)(void *ptr); ///函数指针，该函数的意义就是拷贝当前节点的值
    void (*free)(void *ptr); ///函数指针，该函数的意义就是释放当前节点
    int (*match)(void *ptr, void *key); ///函数指针，比较两个节点的值是否相等
    unsigned long len; ///该链表中节点的个数（链表长度）
} list;

///下面是k宏定义的方法
#define listLength(l) ((l)->len) ///获取一个链表中节点的个数（链表长度）
#define listFirst(l) ((l)->head) ///获取链表的头节点
#define listLast(l) ((l)->tail) ///获取链表的尾节点
#define listPrevNode(n) ((n)->prev) ///获取n节点的前驱节点
#define listNextNode(n) ((n)->next) ///获取n节点的后继节点
#define listNodeValue(n) ((n)->value) ///获取n节点的值

#define listSetDupMethod(l,m) ((l)->dup = (m)) ///这里是为函数指针设置值，也就是给复制这个函数指针设置一个具体实现的方法m
#define listSetFreeMethod(l,m) ((l)->free = (m)) ///这里是为函数指针设置值，也就是给释放这个函数指针设置一个具体实现的方法m
#define listSetMatchMethod(l,m) ((l)->match = (m)) ///这里是为函数指针设置值，也就是给比较这个函数指针设置一个具体实现的方法m

#define listGetDupMethod(l) ((l)->dup) ///获取链表复制函数
#define listGetFreeMethod(l) ((l)->free) ///获取链表释放函数
#define listGetMatchMethod(l) ((l)->match) ///获取链表比较函数

/* Prototypes */
list *listCreate(void); ///创建一个空链表，也就是只有链表头
void listRelease(list *list); ///释放链表list
void listEmpty(list *list); ///将链表置空
list *listAddNodeHead(list *list, void *value); ///在链表的头部添加一个元素
list *listAddNodeTail(list *list, void *value); ///在链表的尾部添加一个元素
list *listInsertNode(list *list, listNode *old_node, void *value, int after); ///在old_node的前后位置添加一个元素，具体添加在前面还是后面，需要由after来确定
void listDelNode(list *list, listNode *node); ///从链表中删除画元素node
listIter *listGetIterator(list *list, int direction); ///为链表创建一个迭代器
listNode *listNext(listIter *iter); ///利用迭代器的方式遍历链表，返回iter当前位置的节点
void listReleaseIterator(listIter *iter); ///释放iterator迭代器
list *listDup(list *orig); ///拷贝orgin链表
listNode *listSearchKey(list *list, void *key); ///在b链表中查询key节点
listNode *listIndex(list *list, long index); ///获取index处的链表节点
void listRewind(list *list, listIter *li); ///将迭代器li重置为到链表的头节点，并进行正向迭代
void listRewindTail(list *list, listIter *li); ///将迭代器li重置为链表的尾节点，进行反向迭代
void listRotateTailToHead(list *list); ///把尾节点作为头节点
void listRotateHeadToTail(list *list); ///把头节点作为尾节点
void listJoin(list *l, list *o); ///合并两个链表

/* Directions for iterators */
#define AL_START_HEAD 0 ///从头到尾进行遍历
#define AL_START_TAIL 1 ///从尾到头进行遍历

#endif /* __ADLIST_H__ */
