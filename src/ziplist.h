#ifndef _ZIPLIST_H
#define _ZIPLIST_H

#define ZIPLIST_HEAD 0  
#define ZIPLIST_TAIL 1

unsigned char *ziplistNew(void); ///创建一个空的压缩表
unsigned char *ziplistMerge(unsigned char **first, unsigned char **second); ///合并两个压缩表
unsigned char *ziplistPush(unsigned char *zl, unsigned char *s, unsigned int slen, int where); ///在压缩表的头或者尾部插入一个节点
unsigned char *ziplistIndex(unsigned char *zl, int index); ///查找index下标处的节点
unsigned char *ziplistNext(unsigned char *zl, unsigned char *p);///获取p指向节点的下一个节点
unsigned char *ziplistPrev(unsigned char *zl, unsigned char *p); ///获取p指向节点的前驱节点
unsigned int ziplistGet(unsigned char *p, unsigned char **sval, unsigned int *slen, long long *lval); ///获取p所指向的节点信息
unsigned char *ziplistInsert(unsigned char *zl, unsigned char *p, unsigned char *s, unsigned int slen); ///在p所指的位置插入节点，如果p在压缩表，则插入p前面
unsigned char *ziplistDelete(unsigned char *zl, unsigned char **p); //删除p所指的位置的节点
unsigned char *ziplistDeleteRange(unsigned char *zl, int index, unsigned int num); ///删除从index处开始的num个节点
unsigned int ziplistCompare(unsigned char *p, unsigned char *s, unsigned int slen);  ///比较p所指的节点和s
unsigned char *ziplistFind(unsigned char *p, unsigned char *vstr, unsigned int vlen, unsigned int skip); ///压缩表中寻找和vstr值相等的节点
unsigned int ziplistLen(unsigned char *zl); ///获取压缩表的长度
size_t ziplistBlobLen(unsigned char *zl); ///获取压缩表的二进制长度
void ziplistRepr(unsigned char *zl);

#ifdef REDIS_TEST
int ziplistTest(int argc, char *argv[]);
#endif

#endif /* _ZIPLIST_H */
