/*
 * redis中双向链表的实现， 其函数的定义在list.h中
 */
#include <stdlib.h>
#include "adlist.h"
#include "zmalloc.h"

/*
 * 创建一个新列表。 可以使用AlFreeList（）释放创建的列表，但是在调用AlFreeList（）之前，用户需要释放每个节点的私有值。
 * 错误时，返回NULL。 否则，指向新列表的指针。
 */
list *listCreate(void)
{
    struct list *list; ///声明一个要创建的list的指针

    if ((list = zmalloc(sizeof(*list))) == NULL) ///为list申请内存空间
        return NULL;
    list->head = list->tail = NULL; ///初始化链表的头节点与尾节点，均为NULL
    list->len = 0; ///初始化链表中元素的个数
    list->dup = NULL; ///初始化复制函数指针
    list->free = NULL; ///初始化释放函数指针
    list->match = NULL;///初始化比较函数指针
    return list;
}

///从列表中删除所有元素，而不破坏列表本身。
void listEmpty(list *list)
{
    unsigned long len;
    listNode *current, *next;

    current = list->head;
    len = list->len;
    while(len--) { ///通过len == 0来判断是否移除元素完毕
        next = current->next;
        if (list->free) list->free(current->value); ///调用list的free函数来释放当前节点的值
        zfree(current); ///释放值后，还需要释放当前节点占用的内存
        current = next; ///将指针指向下一个节点
    }
    list->head = list->tail = NULL; ///将头节点和尾节点置空
    list->len = 0; ///将链表长度置空
}

///释放整个列表。此功能不会失败。
void listRelease(list *list)
{
    listEmpty(list); ///删除链表头已经其中的节点
    zfree(list); ///释放list占用的内存
}

/*
 * 将新节点添加到列表的开头，其中包含指定的“值”指针作为值。
 *
 * 错误时，将返回NULL并且不执行任何操作（即列表保持不变）。
 * 成功后，将返回传递给函数的“列表”指针。
 */
list *listAddNodeHead(list *list, void *value)
{
    listNode *node;

    if ((node = zmalloc(sizeof(*node))) == NULL)///申请内存空间
        return NULL;
    node->value = value; ///为节点赋值
    if (list->len == 0) { ///如果当前链表中还没有原属，list的hhead和tail均指向node
        list->head = list->tail = node;
        node->prev = node->next = NULL;
    } else { ///如果链表中有节点，将node置为头节点，将原来的node置为node的下一个节点
        node->prev = NULL;
        node->next = list->head;
        list->head->prev = node;
        list->head = node;
    }
    list->len++; ///更新链表长度
    return list; ///返回
}


/*
 * 在列表的末尾添加一个新节点，其中包含指定的“值”指针作为值。
 *
 * 错误时，将返回NULL并且不执行任何操作（即列表保持不变）。
 * 成功后，将返回传递给函数的“列表”指针。
 */
///其操作和上一个删除相同，这里就不再详细讲述了
list *listAddNodeTail(list *list, void *value)
{
    listNode *node;

    if ((node = zmalloc(sizeof(*node))) == NULL)
        return NULL;
    node->value = value;
    if (list->len == 0) {
        list->head = list->tail = node;
        node->prev = node->next = NULL;
    } else { ///如果链表中有元素，将node置为尾节点，将原来的尾节点置为node的上一个节点
        node->prev = list->tail;
        node->next = NULL;
        list->tail->next = node;
        list->tail = node;
    }
    list->len++;
    return list;
}

///在old_node的前后位置添加一个元素，具体添加在前面还是后面，需要由after来确定
list *listInsertNode(list *list, listNode *old_node, void *value, int after) {
    listNode *node;

    if ((node = zmalloc(sizeof(*node))) == NULL) ///为新的节点元素申请内存空间
        return NULL;
    node->value = value; ///设置该节点的值
    if (after) { ///如果after > 0，表示在old_node后面添加一新的原属
        node->prev = old_node;
        node->next = old_node->next;
        if (list->tail == old_node) { ///如果old_node是尾节点，则直接将node置为新的尾节点
            list->tail = node;
        }
    } else { ///否则在node的前面添加一个元素
        node->next = old_node;
        node->prev = old_node->prev;
        if (list->head == old_node) { ///如果old_node是头节点，则直接将node置为新的头节点
            list->head = node;
        }
    }
    if (node->prev != NULL) { ///更新node的前驱节点
        node->prev->next = node;
    }
    if (node->next != NULL) { ///更新node的后继节点
        node->next->prev = node;
    }
    list->len++; ///将节点的计算 +1
    return list;
}

/*
 * 从指定列表中删除指定的节点。 调用者可以释放节点的私有值。
 *
 * 此功能不会失败。
 */
void listDelNode(list *list, listNode *node)
{
    if (node->prev) ///如果node不是头节点，更新node的前驱节点的next指针
        node->prev->next = node->next;
    else ///如果是头节点，就直接更新头节点指针即可
        list->head = node->next;
    if (node->next) ///如果node不是尾节点，更新node的后继节点的prev指针
        node->next->prev = node->prev;
    else///如果是尾节点，就直接更新尾节点指针即可
        list->tail = node->prev;
    if (list->free) list->free(node->value); ///释放当前节点值
    zfree(node); ///释放节点内存
    list->len--; ///将节点计算 -1
}

/*
 * 返回列表迭代器“ iter”。 初始化之后，每次调用listNext（）都会返回列表的下一个元素。
 *
 * 此功能不会失败。
 */
listIter *listGetIterator(list *list, int direction)
{
    listIter *iter;

    if ((iter = zmalloc(sizeof(*iter))) == NULL) return NULL; //申请iter内存
    if (direction == AL_START_HEAD) ///如果是从头到尾进行遍历
        iter->next = list->head; ///iter指向list的头节点
    else ///如果是从尾到头进行遍历，
        iter->next = list->tail;///iter指向list的尾节点
    iter->direction = direction; ///设置迭代的方向
    return iter;
}

///释放迭代器内存
void listReleaseIterator(listIter *iter) {
    zfree(iter);
}

///重置迭代器，将li的方向重置为从头到尾，并将li指向list的头节点
void listRewind(list *list, listIter *li) {
    li->next = list->head; ///重置li指向的节点
    li->direction = AL_START_HEAD; ///重置其方向
}

///重置迭代器，将li的方向重置为从头到尾，并将li指向list的尾节点
void listRewindTail(list *list, listIter *li) {
    li->next = list->tail;
    li->direction = AL_START_TAIL;
}

/*
 * 返回迭代器的下一个元素。 使用listDelNode（）删除当前返回的元素是有效的，但不能删除其他元素。
 * 该函数返回一个指向列表中下一个元素的指针，如果没有更多元素，则返回NULL，因此经典用法如下：
 *
 * iter = listGetIterator（list，<direction>）;
 * while（（node = listNext（iter））！= NULL）{
 *     doSomethingWith（listNodeValue（node））;
 * }
 */
listNode *listNext(listIter *iter)
{
    listNode *current = iter->next; ///后去iter指向的当前元素

    if (current != NULL) { ///如果不为空
        if (iter->direction == AL_START_HEAD) ///遍历方向为从头到尾，
            iter->next = current->next; ///就遍历下一个元素
        else
            iter->next = current->prev; ///如果方向为从尾到头，就遍历current的前一个元素
    }
    return current;
}

/*
 * 复制整个列表。 在内存不足时返回NULL。 成功后，将返回原始列表的副本。
 * 通过listSetDupMethod（）函数设置的'Dup'方法用于复制节点值。 否则，将使用与原始节点相同的指针值作为复制节点的值。
 *
 * 无论成功还是错误的原始列表都不会被修改。
 */
list *listDup(list *orig)
{
    list *copy; ///赋值的副本
    listIter iter; ///迭代器声明
    listNode *node; ///用来遍历的临时节点

    if ((copy = listCreate()) == NULL) ///如果创建副本失败，则直接返回
        return NULL;
    copy->dup = orig->dup; ///将copy的复制函数指针指向orig的复制函数
    copy->free = orig->free;///将copy的释放函数指针指向orig的复制释放
    copy->match = orig->match;///将copy的比较函数指针指向orig的比较函数
    listRewind(orig, &iter); ///重置迭代器，采用从头到尾的方式遍历
    while((node = listNext(&iter)) != NULL) { ///遍历整个链表
        void *value;

        if (copy->dup) { ///如果copy的dup函数存在
            value = copy->dup(node->value); ///复制orig的值到copy中
            if (value == NULL) {
                listRelease(copy); ///如果值为空，就释放副本
                return NULL;
            }
        } else ///如果copy的函数不存在，就直接获取当前节点的值
            value = node->value;
        if (listAddNodeTail(copy, value) == NULL) {///将node节点插入到copy的尾部
            listRelease(copy); ///如果失败，就直接返回
            return NULL;
        }
    }
    return copy; ///返回拷贝的副本
}

/*
 * 在列表中搜索与给定键匹配的节点。 匹配使用listSetMatchMethod（）设置的“ match”方法执行。
 * 如果未设置“匹配”方法，则将每个节点的“值”指针与“键”指针直接进行比较。
 * 成功后，将返回第一个匹配的节点指针（从头开始搜索）。 如果不存在匹配的节点，则返回NULL。
 */
listNode *listSearchKey(list *list, void *key)
{
    listIter iter;
    listNode *node;

    listRewind(list, &iter); ///采用从头到尾的方式遍历链表
    while((node = listNext(&iter)) != NULL) { ///如果当前节点不为null
        if (list->match) { ///判断list的比较函数是否存在
            if (list->match(node->value, key)) { ///如果存在就利用它比较两个节点的值
                return node;
            }
        } else { ///如果不存在就直接进行比较
            if (key == node->value) {
                return node;
            }
        }
    }
    return NULL;
}

/*
 * 返回指定的从零开始的索引处的元素，其中0是head，1是head旁边的元素，依此类推。 为了从尾数开始使用负整数，-1是最后一个元素，-2是倒数第二个
 * 等等。 如果索引超出范围，则返回NULL。
 */
listNode *listIndex(list *list, long index) {
    listNode *n;

    if (index < 0) {
        index = (-index)-1;
        n = list->tail;
        while(index-- && n) n = n->prev;
    } else {
        n = list->head;
        while(index-- && n) n = n->next;
    }
    return n;
}

/// 旋转列表，删除尾节点并将其插入到头部。
void listRotateTailToHead(list *list) {
    if (listLength(list) <= 1) return;

    /* Detach current tail */
    listNode *tail = list->tail;
    list->tail = tail->prev;
    list->tail->next = NULL;
    /* Move it as head */
    list->head->prev = tail;
    tail->prev = NULL;
    tail->next = list->head;
    list->head = tail;
}

/// 旋转列表，删除头节点并将其插入尾部。
void listRotateHeadToTail(list *list) {
    if (listLength(list) <= 1) return;

    listNode *head = list->head;
    /* Detach current head */
    list->head = head->next;
    list->head->prev = NULL;
    /* Move it as tail */
    list->tail->next = head;
    head->next = NULL;
    head->prev = list->tail;
    list->tail = head;
}

///在列表“ l”的末尾添加列表“ o”的所有元素。 列表“其他”保持为空，但其他方式有效。
void listJoin(list *l, list *o) {
    if (o->head) ///如果o的head不为NULL
        o->head->prev = l->tail;///让o的head节点指向l的尾部

    if (l->tail) ///如果l的尾节点存在
        l->tail->next = o->head; ///让l的尾节点指向o的头节点
    else
        l->head = o->head; //如果l的尾节点n为null（表示链表为空），则让l的head直接指向o的头节点即可

    if (o->tail) l->tail = o->tail; ///如果o的尾节点不为null，则将l的尾节点指针指向o的尾节点
    l->len += o->len; ///更新链表的长度

    /* Setup other as an empty list. */
    o->head = o->tail = NULL; ///置空链表o的头节点和尾节点
    o->len = 0; ///设置o的len长度0
}
