#ifndef __INTSET_H
#define __INTSET_H
#include <stdint.h>

///整数集合的数据结构定义
typedef struct intset {
    uint32_t encoding; ///编码格式
    uint32_t length; ///计数集合的元素b个数
    int8_t contents[]; ///集合中的具体的元素，是一个柔性数组
} intset;

intset *intsetNew(void); ///创建一个新的整数集合
intset *intsetAdd(intset *is, int64_t value, uint8_t *success); ///将value加入到集合中，success为是否插入成功的标志
intset *intsetRemove(intset *is, int64_t value, int *success); ///从集合中移除原属value，success为是否移除成功的标志
uint8_t intsetFind(intset *is, int64_t value); ///在集合中查找value
int64_t intsetRandom(intset *is); ///在集合中随机查询一个元素
uint8_t intsetGet(intset *is, uint32_t pos, int64_t *value); ///寻找下标为pos的元素，并将其保存在value中
uint32_t intsetLen(const intset *is); ///获取集合的元素个数
size_t intsetBlobLen(intset *is); ///获取集合的字节长度

#ifdef REDIS_TEST
int intsetTest(int argc, char *argv[]);
#endif

#endif // __INTSET_H
