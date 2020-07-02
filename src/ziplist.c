/* ziplist是经过特殊编码的双向链接列表，旨在提高内存效率。 它存储字符串和整数值，其中整数被编码为实际整数，而不是一系列字符。
 * 它允许在O（1）时间在列表的任一侧进行推和弹出操作。 但是，由于每个操作都需要重新分配zip列表使用的内存，因此实际的复杂性与
 * zip列表使用的内存量有关。
 *
 * ----------------------------------------------------------------------------
 *
 * ZIPLIST总体布局
 * ======================
 *
 * ziplist的总体布局如下：
 *
 *           头部节点          |            真实节点数据       |  结尾标志
 * <zlbytes> <zltail> <zllen> | <entry> <entry> ... <entry> |<zlend>
 *
 * 注意：如果没有另外指定，所有字段都以 little endian存储。
 *
 * <uint32_t zlbytes>是一个无符号整数，用于保存ziplist占用的字节数，包括zlbytes字段本身的四个字节。 需要存储该值，
 * 以便能够调整整个结构的大小，而无需先遍历它。
 *
 * <uint32_t zltail>是列表中最后一个条目的偏移量。 这允许在列表的另一端进行弹出操作，而无需完全遍历。
 *
 * <uint16_t zllen>是条目数。当条目数超过2 ^ 16-2时，此值将设置为2 ^ 16-1，我们需要遍历整个列表以了解其包含多少项。
 *
 * <uint8_t zlend>是代表ziplist末尾的特殊条目。 编码为等于255的单个字节。没有其他普通条目以设置为255的字节开头。
 *
 * ZIPLIST 实体节点
 * ===============
 *
 * ziplist中的每个条目都以包含两个信息的元数据作为前缀。 首先，存储前一个条目的长度，以便能够从后到前遍历列表。
 * 第二，提供条目编码。 它表示条目类型，整数或字符串，对于字符串，还表示字符串有效负载的长度。 因此，完整的条目存储如下：
 *
 * 前一个节点长度 |  编码格式   |  节点数据
 * <prevlen>   | <encoding> | <entry-data>
 *
 * 有时，编码代表条目本身，就像后面将要看到的小整数一样。 在这种情况下，<entry-data>部分丢失了，我们可以这样：
 *
 * 前一个节点长度 |  编码格式
 * <prevlen>   | <encoding>
 *
 * 上一个条目的长度<prevlen>以下列方式进行编码：如果此长度小于254个字节，则它将仅消耗一个字节来表示该长度，这是一个无符号
 * 的8位整数。 当长度大于或等于254时，它将占用5个字节。 第一个字节设置为254（FE），以指示随后的值更大。 其余4个字节将
 * 前一个条目的长度作为值。
 *
 * 因此，实际上，条目是通过以下方式编码的：
 *
 * <prevlen from 0 to 253> <encoding> <entry>
 *
 * 或者，如果先前的条目长度大于253个字节，则使用以下编码：
 *
 * 0xFE <4 bytes unsigned little endian prevlen> <encoding> <entry>
 *
 * 条目的编码字段取决于条目的内容。 当条目是字符串时，编码第一个字节的前2位将保存用于存储字符串长度的编码类型，然后是字符串
 * 的实际长度。 当条目是整数时，前2位都设置为1。接下来的2位用于指定在此标头之后将存储哪种整数。 不同类型和编码的概述如下。 
 * 第一个字节始终足以确定条目的类型。
 *
 * |00pppppp| - 1 byte
 *      长度小于或等于63个字节（6位）的字符串值。
 *      "pppppp" 表示无符号的6位长度.
 *
 * |01pppppp|qqqqqqqq| - 2 bytes
 *      长度小于或等于16383字节（14位）的字符串值。
 *      重要说明：14位数字存储在big endian中。
 *
 * |10000000|qqqqqqqq|rrrrrrrr|ssssssss|tttttttt| - 5 bytes
 *      长度大于或等于16384个字节的字符串值。仅第一个字节后的4个字节表示最大长度为32 ^ 2-1。 第一个字节的低6位未使用且设置为零。
 *      重要说明：32位数字存储在big endian中。
 *
 * |11000000| - 3 bytes
 *      整数编码为int16_t（2个字节）。
 * |11010000| - 5 bytes
 *      整数编码为int32_t（4个字节）。
 * |11100000| - 9 bytes
 *      整数编码为int64_t（8个字节）。
 * |11110000| - 4 bytes
 *      整数编码为24位带符号（3个字节）。
 * |11111110| - 2 bytes
 *      整数编码为8位带符号（1个字节）。
 * |1111xxxx| - （xxxx在0000和1101之间）4位整数。
 * 
 * 0到12之间的无符号整数。编码值实际上是1到13，因为不能使用0000和1111，因此应从编码的4位值中减去1以获得正确的值。
 * |11111111| - ziplist特殊条目的结尾。
 *
 * 像ziplist标头一样，所有整数都以little endian字节顺序表示，即使此代码在big endian 系统中编译也是如此。
 *
 * ZIPLISTS 实例：
 * ===========================
 *
 * 下面是一个包含两个表示字符串“ 2”和“ 5”的元素的ziplist。 它由15个字节组成，我们在视觉上将其分为几部分：
 *
 *  [0f 00 00 00] [0c 00 00 00] [02 00] [00 f3] [02 f6] [ff]
 *        |             |          |       |       |     |
 *     zlbytes        zltail    entries   "2"     "5"   end
 *
 * 前4个字节代表数字15，即整个ziplist组成的字节数。后4个字节是找到最后一个ziplist条目的偏移量，即12，实际上，
 * 最后一个条目“ 5”在ziplist内的偏移量12处。下一个16位整数表示ziplist中的元素数，由于其中只有两个元素，因此其
 * 值为2。最后，“ 00 f3”是代表数字2的第一个条目。它由前一个条目长度（由于这是我们的第一个条目）而为零和与编码
 * | 1111xxxx |相对应的字节F3组成。 xxxx在0001和1101之间。我们需要删除“ F”个高阶位1111，并从“ 3”中减去1，
 * 因此输入值为“ 2”。下一个条目的前缀是02，因为第一个条目正好由两个字节组成。条目本身F6的编码方式与第一个条目完全
 * 相同，并且6-1 = 5，因此条目的值为5。最后，特殊条目FF向ziplist的结尾发出信号。
 *
 * 在上面的字符串中添加另一个值为“ Hello World”的元素，使我们能够显示ziplist如何编码小字符串。 我们将仅显示条目
 * 本身的十六进制转储。 想象一下上面的ziplist中存储“ 5”的条目后面的字节：
 *
 * [02] [0b] [48 65 6c 6c 6f 20 57 6f 72 6c 64]
 *
 * 第一个字节02是上一个条目的长度。 下一个字节代表| 00pppppp |模式中的编码 这表示该条目是一个长度<pppppp>的字符串，
 * 因此0B表示其后是11个字节的字符串。 从第三个字节（48）到最后一个字节（64），只有“ Hello World”的ASCII字符。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include "zmalloc.h"
#include "util.h"
#include "ziplist.h"
#include "endianconv.h"
#include "redisassert.h"

#define ZIP_END 255    ///ziplist的结尾标志

///对于每个条目前面的“ prevlen”字段，前一个条目的最大字节数仅用一个字节表示。 否则，它表示为FF AA BB CC DD，
///其中AA BB CC DD是4个字节的无符号整数，表示上一个条目len。
#define ZIP_BIG_PREVLEN 254 

/// 不同的编码/长度的种类
#define ZIP_STR_MASK 0xc0 ///字符串类型的长度 1100 0000
#define ZIP_INT_MASK 0x30 ///INT类型长度 0011 0000
#define ZIP_STR_06B (0 << 6) ///字符串的长度编码 0000 0000
#define ZIP_STR_14B (1 << 6) ///字符串的长度编码 0100 0000
#define ZIP_STR_32B (2 << 6) ///字符串的长度编码 1000 0000
#define ZIP_INT_16B (0xc0 | 0<<4) ///整数类型编码 1100 0000
#define ZIP_INT_32B (0xc0 | 1<<4) ///整数类型编码 1101 0000
#define ZIP_INT_64B (0xc0 | 2<<4) ///整数类型编码 1110 0000
#define ZIP_INT_24B (0xc0 | 3<<4) ///整数类型编码 1111 0000
#define ZIP_INT_8B 0xfe //1111 1110

///4位整数立即编码| 1111xxxx | xxxx在0001和1101之间。
#define ZIP_INT_IMM_MASK 0x0f   ///0000 1111
#define ZIP_INT_IMM_MIN 0xf1    ///1111 0001
#define ZIP_INT_IMM_MAX 0xfd    ///1111 1101 

///24位能够表示的最大整数值和最小整数整数值
#define INT24_MAX 0x7fffff ///0111 1111 1111 1111 1111 1111
#define INT24_MIN (-INT24_MAX - 1) ///1000 0000 0000 0000 0000 0000

///宏定义，用于确定是否为字符串。 字符串从不以“11”作为第一个字节的最高有效位开头。
#define ZIP_IS_STR(enc) (((enc) & ZIP_STR_MASK) < ZIP_STR_MASK)

/* Return total bytes a ziplist is composed of. */
///获取整个ziplist的字节总数，*((uint32_t*)(zl))的意思就是先将zl强制转化为32位的无符号整数，然后通过指针
///就可以访问这个数对应内存的4个字节的信息。
#define ZIPLIST_BYTES(zl) (*((uint32_t*)(zl)))

/* Return the offset of the last item inside the ziplist. */
///获取压缩表终止位置到起始位置的偏移量
#define ZIPLIST_TAIL_OFFSET(zl) (*((uint32_t*)((zl)+sizeof(uint32_t))))

/* Return the length of a ziplist, or UINT16_MAX if the length cannot be
 * determined without scanning the whole ziplist. */
///获取整个压缩表中节点的个数
#define ZIPLIST_LENGTH(zl)      (*((uint16_t*)((zl)+sizeof(uint32_t)*2)))

/* The size of a ziplist header: two 32 bit integers for the total
 * bytes count and last item offset. One 16 bit integer for the number
 * of items field. */
///获取压缩表表头的长度
#define ZIPLIST_HEADER_SIZE     (sizeof(uint32_t)*2+sizeof(uint16_t))

/* Size of the "end of ziplist" entry. Just one byte. */
///获取压缩表结尾标识符的长度
#define ZIPLIST_END_SIZE        (sizeof(uint8_t))

/* Return the pointer to the first entry of a ziplist. */
///获取压缩表节点开始的位置
#define ZIPLIST_ENTRY_HEAD(zl)  ((zl)+ZIPLIST_HEADER_SIZE)

/* Return the pointer to the last entry of a ziplist, using the last entry offset inside the ziplist header. */
///返回压缩表尾节点的位置
#define ZIPLIST_ENTRY_TAIL(zl)  ((zl)+intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl)))

/* Return the pointer to the last byte of a ziplist, which is, the end of ziplist FF entry. */
///返回压缩表结尾标识符的位置（大小为一个字节）
#define ZIPLIST_ENTRY_END(zl)   ((zl)+intrev32ifbe(ZIPLIST_BYTES(zl))-1)

/* 增加ziplist标头中的项目数字段。 请注意，此宏永远不会溢出无符号的16位整数，因为条目总是一次被压入一个。 当达到UINT16_MAX时，
 * 我们希望计数保持在那里，以表示需要进行全面扫描才能获取ziplist中的项目数。 
 */
#define ZIPLIST_INCR_LENGTH(zl,incr) { \   ///增加zipList的长度 
    if (ZIPLIST_LENGTH(zl) < UINT16_MAX) \ ///如果当前ziplist的长度小于uint16的最大值
        ZIPLIST_LENGTH(zl) = intrev16ifbe(intrev16ifbe(ZIPLIST_LENGTH(zl))+incr); \ ///给ziplist的len增加incr
}

///我们使用此功能来接收有关ziplist条目的信息。 请注意，这并不是数据的实际编码方式，而是我们为了方便操作而通过函数填充的内容。
///该结构体定义为ziplist中存储实体的数据结构定义:
 
/// 前一个节点长度 |  编码格式   |  节点数据
/// <prevlen>   | <encoding> | <entry-data>
typedef struct zlentry {
    unsigned int prevrawlensize; ///编码前驱节点的长度所需要的字节大小
    unsigned int prevrawlen;     ///前驱节点的长度
    unsigned int lensize;        ///编码当前节点长度所需要的字节大小
    unsigned int len;            ///当前节点的大小
    unsigned int headersize;     ///当前节点头部长度prevrawlensize + lensize.
    unsigned char encoding;      ///当前节点采用的编码格式
    unsigned char *p;            ///指向当前节点的指针
} zlentry;

#define ZIPLIST_ENTRY_ZERO(zle) { \  ///初始化一个节点
    (zle)->prevrawlensize = (zle)->prevrawlen = 0; \ ///将前驱节点的编码长度、长度设置为0
    (zle)->lensize = (zle)->len = (zle)->headersize = 0; \ ///将该节点的编码长度、长度、头部大小设置为0
    (zle)->encoding = 0; \ ///将该节点的编码格式设置为0
    (zle)->p = NULL; \ ///初始化指向该节点的指针为NULL
}

/// 从“ ptr”指向的字节中提取编码，并将其设置到zlentry结构的“ encoding”字段中。
#define ZIP_ENTRY_ENCODING(ptr, encoding) do {  \
    (encoding) = (ptr[0]); \
    if ((encoding) < ZIP_STR_MASK) (encoding) &= ZIP_STR_MASK; \
} while(0)

/// 返回存储“encoding”编码格式所需要的字节大小。 
unsigned int zipIntSize(unsigned char encoding) {
    switch(encoding) {
    case ZIP_INT_8B:  return 1; ///如果是ZIP_INT_8B，  需要1个节点
    case ZIP_INT_16B: return 2; ///如果是ZIP_INT_16B， 需要2个节点
    case ZIP_INT_24B: return 3; ///如果是ZIP_INT_24B， 需要3个节点
    case ZIP_INT_32B: return 4; ///如果是ZIP_INT_32B， 需要4个节点
    case ZIP_INT_64B: return 8; ///如果是ZIP_INT_64B， 需要8个节点
    }
    if (encoding >= ZIP_INT_IMM_MIN && encoding <= ZIP_INT_IMM_MAX)
        return 0; /* 4 bit immediate */
    panic("Invalid integer encoding 0x%02X", encoding); ///不合法的编码格式
    return 0;
}

/*在'p'中写入数据的encoidng标头。如果p为NULL，则仅返回编码此长度所需的字节数。参数：
 *
 *'encoding'是我们用于该条目的编码。对于单字节小立即数，它可以是ZIP_INT_*或ZIP_STR_*，或者在ZIP_INT_IMM_MIN和ZIP_INT_IMM_MAX之间。
 *
 *'rawlen'仅用于ZIP_STR_*编码，并且是此条目表示的srting的长度。
 *
 *该函数返回存储在“p”中的编码/长度报头使用的字节数。 
 */
unsigned int zipStoreEntryEncoding(unsigned char *p, unsigned char encoding, unsigned int rawlen) {
    unsigned char len = 1, buf[5];

    if (ZIP_IS_STR(encoding)) { ///如果是字符串编码
        
        ///尽管给出了编码，但可能未为字符串设置编码，因此我们在这里使用原始长度来确定它。
        if (rawlen <= 0x3f) { ///如果字符串长度小于或者等于 0x3f
            if (!p) return len; ///如果传入的p为NULL，直接返回len，也就是一个字节
            buf[0] = ZIP_STR_06B | rawlen; ///否则，让buf[0] =  ZIP_STR_06B | rawlen (|为位运算中的或运算符)
        } else if (rawlen <= 0x3fff) {///如果rawlen的长度大于0x3f并且小于0x3fff
            len += 1; ///长度变为两个字节
            if (!p) return len; ///如果p为NULL，返回len，两个字节
            buf[0] = ZIP_STR_14B | ((rawlen >> 8) & 0x3f); ///将编码格式的高8位写入到buf[0]
            buf[1] = rawlen & 0xff;  ///将编码格式的低8为写到buf[1]中
        } else { ///如果rawlen大于0x3fff
            len += 4;  ///将长度变为5个字节
            if (!p) return len; ///如果p为NULL，直接返回len =5
            buf[0] = ZIP_STR_32B; ///将编码格式的高8位写入到buf[0]中
            buf[1] = (rawlen >> 24) & 0xff; ///1111 1111写入buf[1]
            buf[2] = (rawlen >> 16) & 0xff; ///1111 1111写入buf[2]
            buf[3] = (rawlen >> 8) & 0xff;  ///1111 1111写入buf[3]
            buf[4] = rawlen & 0xff;         ///1111 1111写入buf[4]
        }
    } else { ///如果不是字符串编码，那么就应该是整数编码
    
        if (!p) return len; ///整数用一个字节表示，如果p为NULL，直接返回
        buf[0] = encoding; ///将编码格式保存到buf[0]中
    }

    memcpy(p,buf,len); ///将编码格式写入到p所指的地址中
    return len; ///返回编码长度
}

/* 解码以'ptr'编码的条目编码类型和数据长度（字符串的字符串长度，用于整数条目的整数的字节数）。'encoding'变量将保存
 * 条目编码，'lensize'变量将保留 保留编码条目长度所需的字节数，“ len”变量将保留条目长度。 
 */
///从ptr对应的地址空间中去除节点信息，包括encoding、lensize、len
#define ZIP_DECODE_LENGTH(ptr, encoding, lensize, len) do {                                               \
    ZIP_ENTRY_ENCODING((ptr), (encoding)); ///cong ptr中取出编码格式并将它复制给encoding                      \
    if ((encoding) < ZIP_STR_MASK) {  ///如果编码格式为字符串编码                                             \
        if ((encoding) == ZIP_STR_06B) { ///如果是6位的字符串编码，他的编码长度需要一个字节                       \
            (lensize) = 1;                                                                                \
            (len) = (ptr)[0] & 0x3f;  ///将字节长度保存到len中                                               \
        } else if ((encoding) == ZIP_STR_14B) { ///如果是14位字符编码，他的编码长度需要2个字节                  \
            (lensize) = 2;                                                                               \
            (len) = (((ptr)[0] & 0x3f) << 8) | (ptr)[1];  ///将当前的字节长度保存到len中                     \
        } else if ((encoding) == ZIP_STR_32B) { ///如果是32位的编码格式，需此案吗长度需要5个字节                \
            (lensize) = 5;                                                                               \
            (len) = ((ptr)[1] << 24) |                                                                   \
                    ((ptr)[2] << 16) |                                                                   \
                    ((ptr)[3] <<  8) |                                                                   \
                    ((ptr)[4]);     ///将字节长度保存到len中                                                \
        } else {  ///否则，为不合法的编码格式                                                                \
            panic("Invalid string encoding 0x%02X", (encoding));                                         \
        }                                                                                                \
    } else { ///如果不是字符串编码，那就是整数编码                                                             \
        (lensize) = 1; ///整数编码用1个字节保存                                                              \
        (len) = zipIntSize(encoding); ///保存整数的编码的长度                                                \
    }                                                                                                    \
} while(0);

///对p所指的当前节点的前驱节点的长度进行编码，并将其写入“p”。 这仅使用较大的编码（在__ziplistCascadeUpdate中需要）。
int zipStorePrevEntryLengthLarge(unsigned char *p, unsigned int len) {
    if (p != NULL) { ///如果p不为空
        p[0] = ZIP_BIG_PREVLEN; ///这里使用较大的编码，也就是len的字节大于254
        memcpy(p+1,&len,sizeof(len)); ///从p+1处开始拷贝len
        memrev32ifbe(p+1); 
    }
    return 1+sizeof(len); ///返回需要的编码数5
}

///对p所指的当前节点的前驱节点的长度进行编码，并将其写入“p”。 如果“ p”为NULL，则返回编码此长度所需的字节数。
unsigned int zipStorePrevEntryLength(unsigned char *p, unsigned int len) {
    if (p == NULL) { ///如果p为空
        return (len < ZIP_BIG_PREVLEN) ? 1 : sizeof(len)+1; ///如果len小于254，返回1，如果大于等于254，返回5
    } else { ///如果p不为空
        if (len < ZIP_BIG_PREVLEN) { ///如果len小于254
            p[0] = len; ///将len保存到p[0]中
            return 1; ///返回1
        } else { ///如果len大于等于254，则需要调用zipStorePrevEntryLengthLarge()函数进行处理
            return zipStorePrevEntryLengthLarge(p,len);
        }
    }
}

/// 获取ptr节点所指的前驱节点的长度的字节数。 并将保存到变量'prevlensize'中
#define ZIP_DECODE_PREVLENSIZE(ptr, prevlensize) do {                                           \
    if ((ptr)[0] < ZIP_BIG_PREVLEN) { ///如果前驱节点长度小于254，就用1个字节来编码，否则需要5个字节编码  \
        (prevlensize) = 1;                                                                      \
    } else {                                                                                    \
        (prevlensize) = 5;                                                                      \
    }                                                                                           \
} while(0);

///获取ptr节点所指的前驱节点的长度，以及用于编码前一个元素长度的字节数。“ptr”必须指向条目的前缀（前驱节点的长度，
///以便向后遍历元素）。前一个节点的长度存储在“prevlen”中，编码前一个节点长度所需的字节数保存在“prevlensize”中。
#define ZIP_DECODE_PREVLEN(ptr, prevlensize, prevlen) do {                                     \
    ZIP_DECODE_PREVLENSIZE(ptr, prevlensize); ///从ptr所指的地中取出编码前驱节点所需要的字节数        \
    if ((prevlensize) == 1) { ///如果前驱节点的编码长度为                                          \
        (prevlen) = (ptr)[0]; ///从ptr[0]取出值                                                 \
    } else if ((prevlensize) == 5) { ///如果前驱节点编码长度                                      \
        assert(sizeof((prevlen)) == 4); ///因为ptr[0]是标示0xfe                                 \
        memcpy(&(prevlen), ((char*)(ptr)) + 1, 4); ///将值拷贝到prelen中                        \
        memrev32ifbe(&prevlen);                                                               \
    }                                                                                         \
} while(0);

/* 给定指向节点前缀的prevlen信息的指针'p'，如果前一个节点的大小发生变化，此函数将返回编码prevlen所需的字节数之差。
 * 因此，如果A是现在用于编码“prevlen”字段的字节数。
 * B是如果将前一个元素更新为大小为“len”的元素，则对“prevlen”进行编码所需的字节数。
 * 然后函数返回B-A
 * 因此，如果需要更多空间，则函数返回正数；如果需要较少空间，则函数返回负数；如果需要相同空间，则函数返回零。 
 */
int zipPrevLenByteDiff(unsigned char *p, unsigned int len) {
    unsigned int prevlensize; 
    ZIP_DECODE_PREVLENSIZE(p, prevlensize); ///获取前驱节点所需要的字节数大小
    return zipStorePrevEntryLength(NULL, len) - prevlensize; ///计算len所需要的字节数大小减去原来的字节数大小
}

///返回'p'指向的节点所使用的字节总数。 
unsigned int zipRawEntryLength(unsigned char *p) {
    unsigned int prevlensize, encoding, lensize, len; ///声明节点中的各个变量
    ZIP_DECODE_PREVLENSIZE(p, prevlensize); ///返回前驱节点的字节数，并保存到prelensize中
    ZIP_DECODE_LENGTH(p + prevlensize, encoding, lensize, len);///取出当前节点的编码格式，编码节点所需要的字节数，节点的长度
    return prevlensize + lensize + len; ///返回当前节点的大小，它是有前驱节点的大小+当前节点编码所需要的字节数+节点长度
}

///检查'entry'指向的字符串是否可以编码为整数。将整数值存储在“v”中，并将其编码存储在“encoding”中。 
int zipTryEncoding(unsigned char *entry, unsigned int entrylen, long long *v, unsigned char *encoding) {
    
  long long value;
    if (entrylen >= 32 || entrylen == 0) return 0; ///如果节点太长，超过了long long所能表示的范围，或者太短为0，就直接返回
    if (string2ll((char*)entry,entrylen,&value)) { ///将entry字符串转化为long long类型的整数病保存在value中
        
      ///很好，可以对字符串进行编码。 请检查可以容纳此值的最小编码类型。
        if (value >= 0 && value <= 12) { ///如果value的值大于0并且小于12
            *encoding = ZIP_INT_IMM_MIN+value; ///就将其编码格式设置为ZIP_INT_IMM_MIN+value
        } else if (value >= INT8_MIN && value <= INT8_MAX) { ///如果vlaue能用8位表示
            *encoding = ZIP_INT_8B; ///将它的编码格式设置为ZIP_INT_8B
        } else if (value >= INT16_MIN && value <= INT16_MAX) {///如果vlaue能用16位表示
            *encoding = ZIP_INT_16B;///将它的编码格式设置为ZIP_INT_16B
        } else if (value >= INT24_MIN && value <= INT24_MAX) {///如果vlaue能用24位表示
            *encoding = ZIP_INT_24B;///将它的编码格式设置为ZIP_INT_24B
        } else if (value >= INT32_MIN && value <= INT32_MAX) {///如果vlaue能用32位表示
            *encoding = ZIP_INT_32B;///将它的编码格式设置为ZIP_INT_32B
        } else { ///如果是其他的情况，就用64位来表示
            *encoding = ZIP_INT_64B;///将它的编码格式设置为ZIP_INT_64B
        }
        *v = value; ///将value的值赋值给v
        return 1; ///返回1
    }
    return 0;
}

/// 将整数'value'存储在'p'中，编码为'encoding'
void zipSaveInteger(unsigned char *p, int64_t value, unsigned char encoding) {
    int16_t i16;
    int32_t i32;
    int64_t i64;
    if (encoding == ZIP_INT_8B) { ///如果编码格式为ZIP_INT_8B
        ((int8_t*)p)[0] = (int8_t)value; ///讲这个数强制转化为int8保存到p[0]中
    } else if (encoding == ZIP_INT_16B) {///如果编码格式为ZIP_INT_16B
        i16 = value; ///将value保存到i16中
        memcpy(p,&i16,sizeof(i16)); ///讲i16中的值复制到p中
        memrev16ifbe(p);
    } else if (encoding == ZIP_INT_24B) {///如果编码格式为ZIP_INT_24B
        i32 = value<<8;
        memrev32ifbe(&i32);
        memcpy(p,((uint8_t*)&i32)+1,sizeof(i32)-sizeof(uint8_t));
    } else if (encoding == ZIP_INT_32B) {///如果编码格式为ZIP_INT_32B
        i32 = value;
        memcpy(p,&i32,sizeof(i32));
        memrev32ifbe(p);
    } else if (encoding == ZIP_INT_64B) {///如果编码格式为ZIP_INT_64B
        i64 = value;
        memcpy(p,&i64,sizeof(i64));
        memrev64ifbe(p);
    } else if (encoding >= ZIP_INT_IMM_MIN && encoding <= ZIP_INT_IMM_MAX) {
       
      /// 无需执行任何操作，该值存储在编码本身中。 
    } else {
        assert(NULL);
    }
}

/// 从'p'中读取编码为'encoding'的整数
int64_t zipLoadInteger(unsigned char *p, unsigned char encoding) {
    int16_t i16;
    int32_t i32;
    int64_t i64, ret = 0;
    if (encoding == ZIP_INT_8B) { ///如果编码格式为ZIP_INT_8B
        ret = ((int8_t*)p)[0]; ///则直接返回p[0]位置的数据即可
    } else if (encoding == ZIP_INT_16B) { //////如果编码格式为ZIP_INT_16B
        memcpy(&i16,p,sizeof(i16)); ///需要从p开始，读取i16长度，然后将这段内存表示的值保存到i16中
        memrev16ifbe(&i16);
        ret = i16; ///讲结果赋值给ret
    } else if (encoding == ZIP_INT_32B) {
        memcpy(&i32,p,sizeof(i32));
        memrev32ifbe(&i32);
        ret = i32;
    } else if (encoding == ZIP_INT_24B) {
        i32 = 0;
        memcpy(((uint8_t*)&i32)+1,p,sizeof(i32)-sizeof(uint8_t));
        memrev32ifbe(&i32);
        ret = i32>>8;
    } else if (encoding == ZIP_INT_64B) {
        memcpy(&i64,p,sizeof(i64));
        memrev64ifbe(&i64);
        ret = i64;
    } else if (encoding >= ZIP_INT_IMM_MIN && encoding <= ZIP_INT_IMM_MAX) {///如果编码在ZIP_INT_IMM_MIN和ZIP_INT_IMM_MAX之间
        
        ///ZIP_INT_IMM_MIN值为1， 编码时加上了1，所以最后需要减去1
        ret = (encoding & ZIP_INT_IMM_MASK)-1;
    } else {
        assert(NULL);
    }
    return ret;
}

///返回一个节点，其中包含有关节点的所有信息。 
void zipEntry(unsigned char *p, zlentry *e) {

    ZIP_DECODE_PREVLEN(p, e->prevrawlensize, e->prevrawlen); ///获取p所指的节点的前驱节点的编码所需要的字节数以及长度
    ZIP_DECODE_LENGTH(p + e->prevrawlensize, e->encoding, e->lensize, e->len); ///获取当前的编码格式，编码所需要的字节数，以及长度
    e->headersize = e->prevrawlensize + e->lensize; ///设置节点的头部
    e->p = p; ///设置当前节点的指针
}

///创建一个新的空压缩表
unsigned char *ziplistNew(void) {
    unsigned int bytes = ZIPLIST_HEADER_SIZE+ZIPLIST_END_SIZE; ///获取表头大小和表尾大小的和
    unsigned char *zl = zmalloc(bytes); ///申请内存空间
    ZIPLIST_BYTES(zl) = intrev32ifbe(bytes); ///bytes成员初始化
    ZIPLIST_TAIL_OFFSET(zl) = intrev32ifbe(ZIPLIST_HEADER_SIZE); ///初始化为节点偏移量，将其值设置为头结点的大小
    ZIPLIST_LENGTH(zl) = 0; ///节点的长度为0
    zl[bytes-1] = ZIP_END; ///将最后一个字节设置为0xff
    return zl; 
}

///调整压缩列表的大小。
unsigned char *ziplistResize(unsigned char *zl, unsigned int len) {
    zl = zrealloc(zl,len); ///重新给zl分配空间，
    ZIPLIST_BYTES(zl) = intrev32ifbe(len); ///更新zl的总字节数
    zl[len-1] = ZIP_END; ///修改zl的结尾标识符的位置
    return zl; 
}

/* 插入节点导致ziplist产生大的抖动：
 *
 * 插入节点时，我们需要将下一个节点的prevlen字段设置为插入节点的长度。 可能会发生此该节点长度无法以1字节编码的情况，并且下一个
 * 节点需要增大一点以容纳5字节编码的prevlen。 这可以自动完成，因为这仅在已经插入节点时发生（这会导致重新分配和移动）。 但是，
 * 编码prevlen可能会要求该节点也要增长。 当存在大小接近ZIP_BIG_PREVLEN的连续节点时，此效果可能会遍及整个ziplist，因此我
 * 们需要检查prevlen是否可以在每个连续节点中进行编码。
 *
 * 请注意，这种效果也可以反向发生，编码prevlen字段所需的字节可能会缩小，导致后面的节点都缩小，这样就会导致压缩表“抖动”，
 * 针对于这种情况，采取的措施就是不进行处理，让prevlen长度比需要的长。所以最终是要处理上面的那种情况。
 *
 * 指针“p”指向不需要更新的第一个节点，它是对后续节点的检查，而不是当前所指的节点，因为当前这个节点在插入的时候已经已经完成了各项数据填充
 * 已经内存空间的分配。
 */
unsigned char *__ziplistCascadeUpdate(unsigned char *zl, unsigned char *p) {
    size_t curlen = intrev32ifbe(ZIPLIST_BYTES(zl)), rawlen, rawlensize; ///获取当前节点保存总的字节数
    size_t offset, noffset, extra;
    unsigned char *np;
    zlentry cur, next;

    while (p[0] != ZIP_END) { ///遍历这个压缩表，只要没有遇到结尾标识符，就一直遍历下去
        zipEntry(p, &cur); ///将p所指的内存的节点信息保存到cur结构体中
        rawlen = cur.headersize + cur.len; ///如果当前节点的长度
        rawlensize = zipStorePrevEntryLength(NULL,rawlen);///计算当前节点编码所需要的字节数

        /* Abort if there is no next entry. */
        if (p[rawlen] == ZIP_END) break; ///如果下一个位置就是结尾标志符，表示已经遍历完压缩表了，直接跳出循环
        zipEntry(p+rawlen, &next); ///否则获取下一个节点，将它保存到next中

        ///如果下一个节点记录的前驱节点长度编码的字节数和当前节点的长度相等，说明下一个节点有足够的空来保存前驱节点的长度的值，所以
        ///新加入这个节点不会发生抖动，直接跳出循环就行了
        if (next.prevrawlen == rawlen) break; 
        
        ///如果下一个节点记录的前驱节点的长度编码字节数和比当前节点的长度要小，插入这个节点的时候，就需要考虑将下一个节点的header进行
        ///扩大操作
        if (next.prevrawlensize < rawlensize) {
            /* The "prevlen" field of "next" needs more bytes to hold
             * the raw length of "cur". */
            offset = p-zl; ///记录当p指针到zl开始位置的偏移量
            extra = rawlensize-next.prevrawlensize; ///计算下一个节点需要扩大的空间字节数
            zl = ziplistResize(zl,curlen+extra); ///调整压缩表的空间
            p = zl+offset; ///调整p指针到zl开始位置的偏移量

            np = p+rawlen; ///获取下一个节点的指针
            noffset = np-zl; ///下一个节点指针的偏移量

            ///一个元素不是tail元素时，更新tail偏移量。
            if ((zl+intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl))) != np) {
                ZIPLIST_TAIL_OFFSET(zl) = ntrev32ifbe(intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl))+extra);
            }

            ///移动next节点到新的位置
            memmove(np+rawlensize, p+next.prevrawlensize, urlen-noffset-next.prevrawlensize-1);
            ///将next节点的header以rawlen进行重新编码，并且更新节点中的prevrawlensize和prevrawlen信息
            zipStorePrevEntryLength(np,rawlen);

            p += rawlen;///将p移动至下一个节点
            curlen += extra; ///更新压缩表的长度
        } else { ///如果下一个节点的prevrawlensize够对当前进行编码，就不进行操作，包括缩小操作
            if (next.prevrawlensize > rawlensize) {
                
                ///如果next为5个字节，但是上一个节点只需要1个字节，会用5个字节空间将1个字节重新编码，但是空间大小不会变
                zipStorePrevEntryLengthLarge(p+rawlen,rawlen);
            } else {
               
                zipStorePrevEntryLength(p+rawlen,rawlen);///如果next保存的前驱节点编码字节数和当前节点一样，就只需要更新数值即可
            }
            ///从这个地方开始，后面的节点都不会发生”抖动“了，  跳出循环即可
            break;
        }
    }
    return zl; ///返回调整后的压缩表
}

///从“p”开始删除连续的num个节点。 返回指向ziplist的指针。 
unsigned char *__ziplistDelete(unsigned char *zl, unsigned char *p, unsigned int num) {
    unsigned int i, totlen, deleted = 0;
    size_t offset;
    int nextdiff = 0;
    zlentry first, tail;

    zipEntry(p, &first); ///将p的信息保存到first中
    for (i = 0; p[0] != ZIP_END && i < num; i++) { ///遍历压缩表，当遇到ZIP_END或者i>=num，跳出循环
        p += zipRawEntryLength(p); ///将p移动到下一个节点开始的位置
        deleted++; ///删除的数加1
    }

    totlen = p-first.p; ///totalen为删除后链表的长度
    if (totlen > 0) { ///如果删除后还剩余有节点
        if (p[0] != ZIP_END) { ///此时p指向最后一个删除的节点后面的那个节点，如果这个是压缩表结束的位置
            
            ///与当前的prevrawlen相比，在该节点中存储prevrawlen可能会增加或减少所需的字节数。 总是有存
            ///储空间，因为它以前是由一个条目存储的，现在该条目已被删除。
            nextdiff = zipPrevLenByteDiff(p,first.prevrawlen);

            ///请注意，当p向后跳转时，总是有空间：如果新的上一个条目很大，则删除的元素之一具有5个字节的prevlen
            ///标头，因此可以确定至少有5个字节的空闲空间，而我们只需要4个字节即可。* /
            p -= nextdiff;
            ///将first的前驱节点的长度信息保存到p节点中
            zipStorePrevEntryLength(p,first.prevrawlen);

            ///更结尾标识符的偏移量
            ZIPLIST_TAIL_OFFSET(zl) = intrev32ifbe(intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl))-totlen);

            ///当尾部包含多个节点时，我们还需要考虑“ nextdiff”。 否则，更改prevlen的大小不会影响* tail *偏移量。
            ///记录当前p所指向的节点信息
            zipEntry(p, &tail);
            ///
            if (p[tail.headersize+tail.len] != ZIP_END) {
                ZIPLIST_TAIL_OFFSET(zl) =intrev32ifbe(intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl))+nextdiff);
            }

            /* Move tail to the front of the ziplist */
            ///移动数据
            memmove(first.p,p, intrev32ifbe(ZIPLIST_BYTES(zl))-(p-zl)-1);
        } else {
            ///所有的节点都已经删除了，当前的节点是表的尾节点，所以只要更新tail的offset即可
            ZIPLIST_TAIL_OFFSET(zl) =
                intrev32ifbe((first.p-zl)-first.prevrawlen);
        }

        ///整理删除节点后的内存空间
        offset = first.p-zl;
        ///调用ziplistResize函数调整内存大小
        zl = ziplistResize(zl, intrev32ifbe(ZIPLIST_BYTES(zl))-totlen+nextdiff);
        ZIPLIST_INCR_LENGTH(zl,-deleted); ///修改压缩表的节点计数器
        p = zl+offset; ///将p指向压缩表的第一个元素

        /* When nextdiff != 0, the raw length of the next entry has changed, so
         * we need to cascade the update throughout the ziplist */
        ///由于删除节点，所以需要检查是否需要进行连锁更新
        if (nextdiff != 0)
            zl = __ziplistCascadeUpdate(zl,p);
    }
    return zl;
}

///在压缩表中插入一个节点（p的位置进行插入）
unsigned char *__ziplistInsert(unsigned char *zl, unsigned char *p, unsigned char *s, unsigned int slen) {
    size_t curlen = intrev32ifbe(ZIPLIST_BYTES(zl)), reqlen; ///获取当前压缩表的总字节数
    unsigned int prevlensize, prevlen = 0;
    size_t offset;
    int nextdiff = 0; ///用来记录是否会导致连锁更新
    unsigned char encoding = 0;
    long long value = 123456789; ///初始化以避免警告。 使用易于查看的值是否由于某种原因而未初始化使用它。
    zlentry tail;

    /* Find out prevlen for the entry that is inserted. */
    if (p[0] != ZIP_END) { ///如果插入的地方不是尾节点
        ZIP_DECODE_PREVLEN(p, prevlensize, prevlen); ///需要那个p指向节点的prevlensize和prevlen
    } else { ///如果是尾节点
        unsigned char *ptail = ZIPLIST_ENTRY_TAIL(zl); ///获取到压缩表的尾节点地址
        if (ptail[0] != ZIP_END) { ///如果p指向的是最后一个节点，而不是压缩表终止符
            prevlen = zipRawEntryLength(ptail); ///获取到最后这个节点的长度，该值将作为插入节点中的prelen
        }
    }

    ///尝试这个节点是否可以被压缩，就是讲字符串转化为整数，如果可以的话，转化的结果保存到value中，讲采用的编码格式保存在encoding中
    if (zipTryEncoding(s,slen,&value,&encoding)) {

        reqlen = zipIntSize(encoding); ///获取编码长度所需要的字节数
    } else {
        
        reqlen = slen; ///如果说不能压缩，就将slen保存到reqlen中
    }
    /* We need space for both the length of the previous entry and
     * the length of the payload. */
    reqlen += zipStorePrevEntryLength(NULL,prevlen);  ///计算保存插入节点的前驱节点长度所需要的字符数
    reqlen += zipStoreEntryEncoding(NULL,encoding,slen); ///记录保存当前节点所需要的字符数

    ///如果说插入的节点不是在压缩表的末尾的位置，需要确认插入后是否会导致压缩表”抖动“
    int forcelarge = 0;
    nextdiff = (p[0] != ZIP_END) ? zipPrevLenByteDiff(p,reqlen) : 0; ///检查p指向的节点是否能够编码新节点的长度
    if (nextdiff == -4 && reqlen < 4) { ///
        nextdiff = 0;
        forcelarge = 1;
    }

    offset = p-zl; ///需要记录这个偏移量，因为进行内存充分配的时候可能会导致节点地址发生改变
    zl = ziplistResize(zl,curlen+reqlen+nextdiff); ///进行大小调整操作
    p = zl+offset; ///重新分配地址后，需要计算出p的新地址

    ///如果有必要，需要移动内存和更新尾节点的偏移量
    if (p[0] != ZIP_END) { ///如果新节点之后还有节点
        /* Subtract one because of the ZIP_END bytes */
        memmove(p+reqlen,p-nextdiff,curlen-offset-1+nextdiff); ///需要移动内存，为新插入节点腾开空间

        ///在下一个节点中编码该条目的原始长度。
        if (forcelarge)
            zipStorePrevEntryLengthLarge(p+reqlen,reqlen);
        else
            zipStorePrevEntryLength(p+reqlen,reqlen);

        ///在压缩表头中更新尾节点的偏移量
        ZIPLIST_TAIL_OFFSET(zl) = intrev32ifbe(intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl))+reqlen);

        /* When the tail contains more than one entry, we need to take
         * "nextdiff" in account as well. Otherwise, a change in the
         * size of prevlen doesn't have an effect on the *tail* offset. */
        ///如果新节点后面还有多个节点，需要将nextdiff也要计算到尾节点的偏移量中
        zipEntry(p+reqlen, &tail); ///
        if (p[reqlen+tail.headersize+tail.len] != ZIP_END) {
            ZIPLIST_TAIL_OFFSET(zl) = intrev32ifbe(intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl))+nextdiff);
        }
    } else {
        ///如果新节点就是尾节点，更新压缩表表头的尾节点的偏移量
        ZIPLIST_TAIL_OFFSET(zl) = intrev32ifbe(p-zl);
    }

    /* When nextdiff != 0, the raw length of the next entry has changed, so
     * we need to cascade the update throughout the ziplist */
    ///如果nextdiff不等于0，表示已经在下一个节点中也发生了更新，所以可能会导致连锁更新
    if (nextdiff != 0) {
        offset = p-zl;
        zl = __ziplistCascadeUpdate(zl,p+reqlen); ///进行连锁更新
        p = zl+offset;
    }

    /* Write the entry */
    p += zipStorePrevEntryLength(p,prevlen); ///将前度节点的prevlen写入到新节点头中
    p += zipStoreEntryEncoding(p,encoding,slen); ///新节点的编码，长度写入到节点头中
    if (ZIP_IS_STR(encoding)) { ///如果编码是字符串类型
        memcpy(p,s,slen); ///就将这个字符串拷贝到p位置
    } else { 
        zipSaveInteger(p,value,encoding); ///如果是整数类型，就将值和编码写入节点中
    }
    ZIPLIST_INCR_LENGTH(zl,1); ///压缩表的节点个数加1
    return zl; 
}

/* 通过将“second”追加到“first”来合并ziplist“ first”和“ second”。
 *
 * 注意：较大的ziplist被重新分配以包含新的合并的ziplist。 结果可以使用“第一”或“第二”。 未使用的参数将被释放并设置为NULL。
 *
 * 调用此函数后，输入参数不再有效，因为它们已被更改并就地释放。
 *
 * 结果ziplist是“first”的内容，然后是“second”。
 *
 * 失败时：如果无法合并，则返回NULL。
 * 成功时：返回合并的ziplist（这是“ first”或“ second”的扩展版本，还释放其他未使用的输入ziplist，并将输入ziplist参数
 * 设置为等于新重新分配的ziplist返回值。 
 */
///合并两个压缩表，将second追加到first后面
unsigned char *ziplistMerge(unsigned char **first, unsigned char **second) {

    if (first == NULL || *first == NULL || second == NULL || *second == NULL) ///如果说只要有一个参数为空，就可以停止合并
        return NULL;

    if (*first == *second) ///如果两个压缩表是同一个压缩表，不能进行合并
        return NULL;

    size_t first_bytes = intrev32ifbe(ZIPLIST_BYTES(*first)); ///获取第一个压缩表的字节数
    size_t first_len = intrev16ifbe(ZIPLIST_LENGTH(*first)); ///获取第一个压缩表的节点数

    size_t second_bytes = intrev32ifbe(ZIPLIST_BYTES(*second)); ///获取第二个压缩表的字节数
    size_t second_len = intrev16ifbe(ZIPLIST_LENGTH(*second)); ///获取第二个压缩表节点数

    int append;
    unsigned char *source, *target;
    size_t target_bytes, source_bytes;
   
    ///选择最大的ziplist，以便我们就地调整大小。 我们还必须跟踪是否正在添加或添加到目标ziplist。
    if (first_len >= second_len) {
        
        ///保留first，second附加到first。
        target = *first;
        target_bytes = first_bytes;
        source = *second;
        source_bytes = second_bytes;
        append = 1; ///标志位，用来标志first为target，second为source
    } else {
        ///保留second，first附加到second。
        target = *second;
        target_bytes = second_bytes;
        source = *first;
        source_bytes = first_bytes;
        append = 0;///标志位，用来标志second为target，first为source
    }

    ///计算最终各项数据的大小
    size_t zlbytes = first_bytes + second_bytes -
                     ZIPLIST_HEADER_SIZE - ZIPLIST_END_SIZE; ///计算合并后所有的字节数
    size_t zllength = first_len + second_len; ///计算合并后压缩表的节点个数

    ///判断合并后的长度是否超过UINT16_MAX，如果超过了将其值设为UINT16_MAX
    zllength = zllength < UINT16_MAX ? zllength : UINT16_MAX;

    ///把两个压缩表的尾节点偏移量记录下来
    size_t first_offset = intrev32ifbe(ZIPLIST_TAIL_OFFSET(*first));
    size_t second_offset = intrev32ifbe(ZIPLIST_TAIL_OFFSET(*second));

    ///从新申请target的内存空间，新的内存空间大小为zlbytes
    target = zrealloc(target, zlbytes);
    if (append) { ///如果是first为target，second为source
        /* append == appending to target */
        /* Copy source after target (copying over original [END]):
         *   [TARGET - END, SOURCE - HEADER] */
        memcpy(target + target_bytes - ZIPLIST_END_SIZE,
               source + ZIPLIST_HEADER_SIZE,
               source_bytes - ZIPLIST_HEADER_SIZE); ///直接将source中的内存拷贝到target后面
    } else { ///如果是second为target，first为source
        /* !append == prepending to target */
        /* Move target *contents* exactly size of (source - [END]),
         * then copy source into vacataed space (source - [END]):
         *   [SOURCE - END, TARGET - HEADER] */
        memmove(target + source_bytes - ZIPLIST_END_SIZE,
                target + ZIPLIST_HEADER_SIZE,
                target_bytes - ZIPLIST_HEADER_SIZE); ///先将second移动到后面的位置
        memcpy(target, source, source_bytes - ZIPLIST_END_SIZE); ///将first写入到second前面
    }

    ///更新头结点的各种信息
    ZIPLIST_BYTES(target) = intrev32ifbe(zlbytes); ///更新压缩表头结点中的zlbytes
    ZIPLIST_LENGTH(target) = intrev16ifbe(zllength); ///更新压缩表头结点中的zllength
    /* New tail offset is:
     *   + N bytes of first ziplist
     *   - 1 byte for [END] of first ziplist
     *   + M bytes for the offset of the original tail of the second ziplist
     *   - J bytes for HEADER because second_offset keeps no header. */
    ZIPLIST_TAIL_OFFSET(target) = intrev32ifbe( 
                                   (first_bytes - ZIPLIST_END_SIZE) +
                                   (second_offset - ZIPLIST_HEADER_SIZE)); ///更新压缩表头结点中的尾节点偏移量

    ///讲second追加到first之后，我们还需要判断是否需要进行连锁更新操作
    target = __ziplistCascadeUpdate(target, target+first_offset); 

    ///释放掉不用的内存
    if (append) { ///如果是first为target，second为source
        zfree(*second); ///释放掉second
        *second = NULL;
        *first = target;
    } else { ///如果是second为target，first为source
        zfree(*first); ///释放掉first
        *first = NULL;
        *second = target;
    }
    return target;
}

///讲长度为slen的字符串push到zl中，where表示push的地方，可以是头部，也可以是尾部
unsigned char *ziplistPush(unsigned char *zl, unsigned char *s, unsigned int slen, int where) {
    unsigned char *p;
    p = (where == ZIPLIST_HEAD) ? ZIPLIST_ENTRY_HEAD(zl) : ZIPLIST_ENTRY_END(zl);
    return __ziplistInsert(zl,p,s,slen); ///进行插入操作
}

/* Returns an offset to use for iterating with ziplistNext. When the given
 * index is negative, the list is traversed back to front. When the list
 * doesn't contain an element at the provided index, NULL is returned. */
///查询下标为index除的节点地址
unsigned char *ziplistIndex(unsigned char *zl, int index) {
    unsigned char *p;
    unsigned int prevlensize, prevlen = 0;
    if (index < 0) { ///如果index小于0，表示倒数第index个节点
        index = (-index)-1; ///计算出它从尾到头的遍历次数
        p = ZIPLIST_ENTRY_TAIL(zl); ///获取到zl的尾节点
        if (p[0] != ZIP_END) { ///如果不是空表
            ZIP_DECODE_PREVLEN(p, prevlensize, prevlen); ///获取上一个节点的prevlensize和prevlen
            while (prevlen > 0 && index--) { ///如果前驱节点存在并且还没有到达索引的位置
                p -= prevlen; ///计算出前驱节点的地址，并修改p指针
                ZIP_DECODE_PREVLEN(p, prevlensize, prevlen); ///更新p中的prevlensize和prevlen
            }
        }
    } else { ///如果index大于0，就从头到尾的方式进行遍历
        p = ZIPLIST_ENTRY_HEAD(zl); ///获取到第一个节点的位置
        while (p[0] != ZIP_END && index--) {  ///如果还没有到尾节点并且也没有到达寻找的下标
            p += zipRawEntryLength(p); ///更新p的值,指向下一个节点
        }
    }
    return (p[0] == ZIP_END || index > 0) ? NULL : p; ///如果找到了就返回，没找到就返回NULL
}

///返回指向ziplist中下一个节点的指针。 zl是指向ziplist的指针，p是指向当前节点的指针，返回'p'的下一个节点，如果不存在返回NULL。 
unsigned char *ziplistNext(unsigned char *zl, unsigned char *p) {
    ((void) zl);

    if (p[0] == ZIP_END) { ///如果p为尾节点，则直接返回NULL
        return NULL;
    }
    p += zipRawEntryLength(p); ///获取到p的下一个节点
    if (p[0] == ZIP_END) { ///如果p已经指向了尾节点,直接返回NULL
        return NULL;
    }
    return p;
}

///返回p的前驱节点
unsigned char *ziplistPrev(unsigned char *zl, unsigned char *p) {
    unsigned int prevlensize, prevlen = 0;

    /* Iterating backwards from ZIP_END should return the tail. When "p" is
     * equal to the first element of the list, we're already at the head,
     * and should return NULL. */
    if (p[0] == ZIP_END) { ///如果p指向结尾标识符，表示列表为空
        p = ZIPLIST_ENTRY_TAIL(zl); ///找到尾节点的位置
        return (p[0] == ZIP_END) ? NULL : p; ///如果说尾节点为节点成员，则直接返回，否则只是一个结尾标识符，返回NULL
    } else if (p == ZIPLIST_ENTRY_HEAD(zl)) { ///如果说p是压缩表的头节点，那个没有前驱节点，直接返回null
        return NULL;
    } else {
        ZIP_DECODE_PREVLEN(p, prevlensize, prevlen); ///如果p是中间的节点，那么获取上一个节点的prevlensize和prevlen
        assert(prevlen > 0);
        return p-prevlen; ///返回前驱节点的地址
    }
}

///获取由'p'指向的节点，并根据节点的编码存储在'*sstr'或'sval'中。 “*sstr”始终设置为NULL，以便能够确定是否设置了字符串指针或整数值。
///如果'p'指向ziplist的末尾，则返回0，否则返回1。
unsigned int ziplistGet(unsigned char *p, unsigned char **sstr, unsigned int *slen, long long *sval) {
    zlentry entry;
    if (p == NULL || p[0] == ZIP_END) return 0; ///如果p为空或者p指向压缩表结尾 标识符，则直接返回
    if (sstr) *sstr = NULL;

    zipEntry(p, &entry); ///将p节点中的信息保存到entry中
    if (ZIP_IS_STR(entry.encoding)) { ///如果采用的字符串编码
        if (sstr) { ///如果sstr不为NULL
            *slen = entry.len; ///返回节点长度
            *sstr = p+entry.headersize; ///返回字符串
        }
    } else { ///如果是整数编码格式
        if (sval) { ///如果sval不为NULL
            *sval = zipLoadInteger(p+entry.headersize,entry.encoding); ///将值保存到sval中
        }
    }
    return 1;
}

///在p所指的位置插入节点，如果p是压缩表中的节点，则讲这个新节点插入到p的前面
unsigned char *ziplistInsert(unsigned char *zl, unsigned char *p, unsigned char *s, unsigned int slen) {
    return __ziplistInsert(zl,p,s,slen);
}

///从ziplist中删除由 *p指向的节点。还要在适当位置更新p，以便在删除条目时可以遍历ziplist。
unsigned char *ziplistDelete(unsigned char *zl, unsigned char **p) {
    size_t offset = *p-zl; ///保存p节点的偏移量
    zl = __ziplistDelete(zl,*p,1); ///进行节点删除，1表示只要删除一个节点即可

    /* Store pointer to current element in p, because ziplistDelete will
     * do a realloc which might result in a different "zl"-pointer.
     * When the delete direction is back to front, we might delete the last
     * entry and end up with "p" pointing to ZIP_END, so check this. */
    *p = zl+offset; ///将p指向新的节点
    return zl;
}

///在压缩表中删除从index开始，num个节点
unsigned char *ziplistDeleteRange(unsigned char *zl, int index, unsigned int num) {
    unsigned char *p = ziplistIndex(zl,index); ///需要先定位index所指的地址
    return (p == NULL) ? zl : __ziplistDelete(zl,p,num); ///如果p不存在则直接返回，否则进行节点删除操作
}

///比较p所指向的节点和sstr，如果相等返回1，不相等返回0
unsigned int ziplistCompare(unsigned char *p, unsigned char *sstr, unsigned int slen) {
    zlentry entry; 
    unsigned char sencoding;
    long long zval, sval;
    if (p[0] == ZIP_END) return 0; ///如果p指向结尾标识符，则直接返回
 
    zipEntry(p, &entry); ///将p节点信息保存到entry中
    if (ZIP_IS_STR(entry.encoding)) { ///如果是字符编码
        /* Raw compare */
        if (entry.len == slen) { ///判断字符串长度是否相等
            return memcmp(p+entry.headersize,sstr,slen) == 0; ///如果长度相等，就进行字符串值比较
        } else {
            return 0; ///如果长度不相等，就返回0
        }
    } else { ///如果是整数编码方式
        /* Try to compare encoded values. Don't compare encoding because
         * different implementations may encoded integers differently. */
        if (zipTryEncoding(sstr,slen,&sval,&sencoding)) { ///如果sstr能够转化为整数
          zval = zipLoadInteger(p+entry.headersize,entry.encoding); ///获取p所指向的节点的整数的值
          return zval == sval; ///进行对比
        }
    }
    return 0; ///如果不相等，统统返回0
}

/* Find pointer to the entry equal to the specified entry. Skip 'skip' entries
 * between every comparison. Returns NULL when the field could not be found. */
///查找和vstr值相等的节点。 在每个比较之间跳过“skip”节点。 找不到字段时返回NULL。
unsigned char *ziplistFind(unsigned char *p, unsigned char *vstr, unsigned int vlen, unsigned int skip) {
    int skipcnt = 0;
    unsigned char vencoding = 0;
    long long vll = 0;

    while (p[0] != ZIP_END) { ///遍历整个压缩表
        unsigned int prevlensize, encoding, lensize, len;
        unsigned char *q;

        ZIP_DECODE_PREVLENSIZE(p, prevlensize); ///获取p指向的节点的head中的prevlensize
        ZIP_DECODE_LENGTH(p + prevlensize, encoding, lensize, len); ///获取当前节点的编码、lensize以及len
        q = p + prevlensize + lensize; ///获取p的后继节点，并且其复制给q

        if (skipcnt == 0) { 
            if (ZIP_IS_STR(encoding)) { ///如果编码格式为字符串编码
                if (len == vlen && memcmp(q, vstr, vlen) == 0) { ///如果两个字符串长度相等，并且字符串内容相等
                    return p; ///返回p
                }
            } else { ///如果是整数编码
                if (vencoding == 0) { ///如果vstr没有进行过编码操作，就进行一次，以后就不在执行了 , 用vencoding来标识
                    if (!zipTryEncoding(vstr, vlen, &vll, &vencoding)) {
     
                        vencoding = UCHAR_MAX;
                    }
                    /* Must be non-zero by now */
                    assert(vencoding);
                }

                /* Compare current entry with specified entry, do it only
                 * if vencoding != UCHAR_MAX because if there is no encoding
                 * possible for the field it can't be a valid integer. */
                if (vencoding != UCHAR_MAX) {
                    long long ll = zipLoadInteger(q, encoding); ///获取q的整数数值
                    if (ll == vll) { ///进行数值比较
                        return p;
                    }
                }
            }
            skipcnt = skip; ///重设skipcnt的值
        } else {
            /* Skip entry */
            skipcnt--; ///跳过skip个节点
        }
        p = q + len;  ///指向下一个节点
    }
    return NULL;
}

///返回压缩表中的节点数量
unsigned int ziplistLen(unsigned char *zl) {
    unsigned int len = 0;
    if (intrev16ifbe(ZIPLIST_LENGTH(zl)) < UINT16_MAX) { ///如果节点的数量少于UINT16_MAX
        len = intrev16ifbe(ZIPLIST_LENGTH(zl));///直接返回len的值
    } else {
        unsigned char *p = zl+ZIPLIST_HEADER_SIZE; ///如果大于UINT16_MAX， 需要采用遍历的开始来进行统计
        while (*p != ZIP_END) {
            p += zipRawEntryLength(p);
            len++;
        }
        ///设置节点的数量
        if (len < UINT16_MAX) ZIPLIST_LENGTH(zl) = intrev16ifbe(len);
    }
    return len;
}

///返回压缩表总的字符数
size_t ziplistBlobLen(unsigned char *zl) {
    return intrev32ifbe(ZIPLIST_BYTES(zl));
}

///格式化打印
void ziplistRepr(unsigned char *zl) {
    unsigned char *p;
    int index = 0;
    zlentry entry;

    printf(
        "{total bytes %d} "
        "{num entries %u}\n"
        "{tail offset %u}\n",
        intrev32ifbe(ZIPLIST_BYTES(zl)),
        intrev16ifbe(ZIPLIST_LENGTH(zl)),
        intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl)));
    p = ZIPLIST_ENTRY_HEAD(zl);
    while(*p != ZIP_END) {
        zipEntry(p, &entry);
        printf(
            "{\n"
                "\taddr 0x%08lx,\n"
                "\tindex %2d,\n"
                "\toffset %5ld,\n"
                "\thdr+entry len: %5u,\n"
                "\thdr len%2u,\n"
                "\tprevrawlen: %5u,\n"
                "\tprevrawlensize: %2u,\n"
                "\tpayload %5u\n",
            (long unsigned)p,
            index,
            (unsigned long) (p-zl),
            entry.headersize+entry.len,
            entry.headersize,
            entry.prevrawlen,
            entry.prevrawlensize,
            entry.len);
        printf("\tbytes: ");
        for (unsigned int i = 0; i < entry.headersize+entry.len; i++) {
            printf("%02x|",p[i]);
        }
        printf("\n");
        p += entry.headersize;
        if (ZIP_IS_STR(entry.encoding)) {
            printf("\t[str]");
            if (entry.len > 40) {
                if (fwrite(p,40,1,stdout) == 0) perror("fwrite");
                printf("...");
            } else {
                if (entry.len &&
                    fwrite(p,entry.len,1,stdout) == 0) perror("fwrite");
            }
        } else {
            printf("\t[int]%lld", (long long) zipLoadInteger(p,entry.encoding));
        }
        printf("\n}\n");
        p += entry.len;
        index++;
    }
    printf("{end}\n\n");
}

#ifdef REDIS_TEST
#include <sys/time.h>
#include "adlist.h"
#include "sds.h"

#define debug(f, ...) { if (DEBUG) printf(f, __VA_ARGS__); }

static unsigned char *createList() {
    unsigned char *zl = ziplistNew();
    zl = ziplistPush(zl, (unsigned char*)"foo", 3, ZIPLIST_TAIL);
    zl = ziplistPush(zl, (unsigned char*)"quux", 4, ZIPLIST_TAIL);
    zl = ziplistPush(zl, (unsigned char*)"hello", 5, ZIPLIST_HEAD);
    zl = ziplistPush(zl, (unsigned char*)"1024", 4, ZIPLIST_TAIL);
    return zl;
}

static unsigned char *createIntList() {
    unsigned char *zl = ziplistNew();
    char buf[32];

    sprintf(buf, "100");
    zl = ziplistPush(zl, (unsigned char*)buf, strlen(buf), ZIPLIST_TAIL);
    sprintf(buf, "128000");
    zl = ziplistPush(zl, (unsigned char*)buf, strlen(buf), ZIPLIST_TAIL);
    sprintf(buf, "-100");
    zl = ziplistPush(zl, (unsigned char*)buf, strlen(buf), ZIPLIST_HEAD);
    sprintf(buf, "4294967296");
    zl = ziplistPush(zl, (unsigned char*)buf, strlen(buf), ZIPLIST_HEAD);
    sprintf(buf, "non integer");
    zl = ziplistPush(zl, (unsigned char*)buf, strlen(buf), ZIPLIST_TAIL);
    sprintf(buf, "much much longer non integer");
    zl = ziplistPush(zl, (unsigned char*)buf, strlen(buf), ZIPLIST_TAIL);
    return zl;
}

static long long usec(void) {
    struct timeval tv;
    gettimeofday(&tv,NULL);
    return (((long long)tv.tv_sec)*1000000)+tv.tv_usec;
}

static void stress(int pos, int num, int maxsize, int dnum) {
    int i,j,k;
    unsigned char *zl;
    char posstr[2][5] = { "HEAD", "TAIL" };
    long long start;
    for (i = 0; i < maxsize; i+=dnum) {
        zl = ziplistNew();
        for (j = 0; j < i; j++) {
            zl = ziplistPush(zl,(unsigned char*)"quux",4,ZIPLIST_TAIL);
        }

        /* Do num times a push+pop from pos */
        start = usec();
        for (k = 0; k < num; k++) {
            zl = ziplistPush(zl,(unsigned char*)"quux",4,pos);
            zl = ziplistDeleteRange(zl,0,1);
        }
        printf("List size: %8d, bytes: %8d, %dx push+pop (%s): %6lld usec\n",
            i,intrev32ifbe(ZIPLIST_BYTES(zl)),num,posstr[pos],usec()-start);
        zfree(zl);
    }
}

static unsigned char *pop(unsigned char *zl, int where) {
    unsigned char *p, *vstr;
    unsigned int vlen;
    long long vlong;

    p = ziplistIndex(zl,where == ZIPLIST_HEAD ? 0 : -1);
    if (ziplistGet(p,&vstr,&vlen,&vlong)) {
        if (where == ZIPLIST_HEAD)
            printf("Pop head: ");
        else
            printf("Pop tail: ");

        if (vstr) {
            if (vlen && fwrite(vstr,vlen,1,stdout) == 0) perror("fwrite");
        }
        else {
            printf("%lld", vlong);
        }

        printf("\n");
        return ziplistDelete(zl,&p);
    } else {
        printf("ERROR: Could not pop\n");
        exit(1);
    }
}

static int randstring(char *target, unsigned int min, unsigned int max) {
    int p = 0;
    int len = min+rand()%(max-min+1);
    int minval, maxval;
    switch(rand() % 3) {
    case 0:
        minval = 0;
        maxval = 255;
    break;
    case 1:
        minval = 48;
        maxval = 122;
    break;
    case 2:
        minval = 48;
        maxval = 52;
    break;
    default:
        assert(NULL);
    }

    while(p < len)
        target[p++] = minval+rand()%(maxval-minval+1);
    return len;
}

static void verify(unsigned char *zl, zlentry *e) {
    int len = ziplistLen(zl);
    zlentry _e;

    ZIPLIST_ENTRY_ZERO(&_e);

    for (int i = 0; i < len; i++) {
        memset(&e[i], 0, sizeof(zlentry));
        zipEntry(ziplistIndex(zl, i), &e[i]);

        memset(&_e, 0, sizeof(zlentry));
        zipEntry(ziplistIndex(zl, -len+i), &_e);

        assert(memcmp(&e[i], &_e, sizeof(zlentry)) == 0);
    }
}

int ziplistTest(int argc, char **argv) {
    unsigned char *zl, *p;
    unsigned char *entry;
    unsigned int elen;
    long long value;

    /* If an argument is given, use it as the random seed. */
    if (argc == 2)
        srand(atoi(argv[1]));

    zl = createIntList();
    ziplistRepr(zl);

    zfree(zl);

    zl = createList();
    ziplistRepr(zl);

    zl = pop(zl,ZIPLIST_TAIL);
    ziplistRepr(zl);

    zl = pop(zl,ZIPLIST_HEAD);
    ziplistRepr(zl);

    zl = pop(zl,ZIPLIST_TAIL);
    ziplistRepr(zl);

    zl = pop(zl,ZIPLIST_TAIL);
    ziplistRepr(zl);

    zfree(zl);

    printf("Get element at index 3:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, 3);
        if (!ziplistGet(p, &entry, &elen, &value)) {
            printf("ERROR: Could not access index 3\n");
            return 1;
        }
        if (entry) {
            if (elen && fwrite(entry,elen,1,stdout) == 0) perror("fwrite");
            printf("\n");
        } else {
            printf("%lld\n", value);
        }
        printf("\n");
        zfree(zl);
    }

    printf("Get element at index 4 (out of range):\n");
    {
        zl = createList();
        p = ziplistIndex(zl, 4);
        if (p == NULL) {
            printf("No entry\n");
        } else {
            printf("ERROR: Out of range index should return NULL, returned offset: %ld\n", p-zl);
            return 1;
        }
        printf("\n");
        zfree(zl);
    }

    printf("Get element at index -1 (last element):\n");
    {
        zl = createList();
        p = ziplistIndex(zl, -1);
        if (!ziplistGet(p, &entry, &elen, &value)) {
            printf("ERROR: Could not access index -1\n");
            return 1;
        }
        if (entry) {
            if (elen && fwrite(entry,elen,1,stdout) == 0) perror("fwrite");
            printf("\n");
        } else {
            printf("%lld\n", value);
        }
        printf("\n");
        zfree(zl);
    }

    printf("Get element at index -4 (first element):\n");
    {
        zl = createList();
        p = ziplistIndex(zl, -4);
        if (!ziplistGet(p, &entry, &elen, &value)) {
            printf("ERROR: Could not access index -4\n");
            return 1;
        }
        if (entry) {
            if (elen && fwrite(entry,elen,1,stdout) == 0) perror("fwrite");
            printf("\n");
        } else {
            printf("%lld\n", value);
        }
        printf("\n");
        zfree(zl);
    }

    printf("Get element at index -5 (reverse out of range):\n");
    {
        zl = createList();
        p = ziplistIndex(zl, -5);
        if (p == NULL) {
            printf("No entry\n");
        } else {
            printf("ERROR: Out of range index should return NULL, returned offset: %ld\n", p-zl);
            return 1;
        }
        printf("\n");
        zfree(zl);
    }

    printf("Iterate list from 0 to end:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, 0);
        while (ziplistGet(p, &entry, &elen, &value)) {
            printf("Entry: ");
            if (entry) {
                if (elen && fwrite(entry,elen,1,stdout) == 0) perror("fwrite");
            } else {
                printf("%lld", value);
            }
            p = ziplistNext(zl,p);
            printf("\n");
        }
        printf("\n");
        zfree(zl);
    }

    printf("Iterate list from 1 to end:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, 1);
        while (ziplistGet(p, &entry, &elen, &value)) {
            printf("Entry: ");
            if (entry) {
                if (elen && fwrite(entry,elen,1,stdout) == 0) perror("fwrite");
            } else {
                printf("%lld", value);
            }
            p = ziplistNext(zl,p);
            printf("\n");
        }
        printf("\n");
        zfree(zl);
    }

    printf("Iterate list from 2 to end:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, 2);
        while (ziplistGet(p, &entry, &elen, &value)) {
            printf("Entry: ");
            if (entry) {
                if (elen && fwrite(entry,elen,1,stdout) == 0) perror("fwrite");
            } else {
                printf("%lld", value);
            }
            p = ziplistNext(zl,p);
            printf("\n");
        }
        printf("\n");
        zfree(zl);
    }

    printf("Iterate starting out of range:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, 4);
        if (!ziplistGet(p, &entry, &elen, &value)) {
            printf("No entry\n");
        } else {
            printf("ERROR\n");
        }
        printf("\n");
        zfree(zl);
    }

    printf("Iterate from back to front:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, -1);
        while (ziplistGet(p, &entry, &elen, &value)) {
            printf("Entry: ");
            if (entry) {
                if (elen && fwrite(entry,elen,1,stdout) == 0) perror("fwrite");
            } else {
                printf("%lld", value);
            }
            p = ziplistPrev(zl,p);
            printf("\n");
        }
        printf("\n");
        zfree(zl);
    }

    printf("Iterate from back to front, deleting all items:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, -1);
        while (ziplistGet(p, &entry, &elen, &value)) {
            printf("Entry: ");
            if (entry) {
                if (elen && fwrite(entry,elen,1,stdout) == 0) perror("fwrite");
            } else {
                printf("%lld", value);
            }
            zl = ziplistDelete(zl,&p);
            p = ziplistPrev(zl,p);
            printf("\n");
        }
        printf("\n");
        zfree(zl);
    }

    printf("Delete inclusive range 0,0:\n");
    {
        zl = createList();
        zl = ziplistDeleteRange(zl, 0, 1);
        ziplistRepr(zl);
        zfree(zl);
    }

    printf("Delete inclusive range 0,1:\n");
    {
        zl = createList();
        zl = ziplistDeleteRange(zl, 0, 2);
        ziplistRepr(zl);
        zfree(zl);
    }

    printf("Delete inclusive range 1,2:\n");
    {
        zl = createList();
        zl = ziplistDeleteRange(zl, 1, 2);
        ziplistRepr(zl);
        zfree(zl);
    }

    printf("Delete with start index out of range:\n");
    {
        zl = createList();
        zl = ziplistDeleteRange(zl, 5, 1);
        ziplistRepr(zl);
        zfree(zl);
    }

    printf("Delete with num overflow:\n");
    {
        zl = createList();
        zl = ziplistDeleteRange(zl, 1, 5);
        ziplistRepr(zl);
        zfree(zl);
    }

    printf("Delete foo while iterating:\n");
    {
        zl = createList();
        p = ziplistIndex(zl,0);
        while (ziplistGet(p,&entry,&elen,&value)) {
            if (entry && strncmp("foo",(char*)entry,elen) == 0) {
                printf("Delete foo\n");
                zl = ziplistDelete(zl,&p);
            } else {
                printf("Entry: ");
                if (entry) {
                    if (elen && fwrite(entry,elen,1,stdout) == 0)
                        perror("fwrite");
                } else {
                    printf("%lld",value);
                }
                p = ziplistNext(zl,p);
                printf("\n");
            }
        }
        printf("\n");
        ziplistRepr(zl);
        zfree(zl);
    }

    printf("Regression test for >255 byte strings:\n");
    {
        char v1[257] = {0}, v2[257] = {0};
        memset(v1,'x',256);
        memset(v2,'y',256);
        zl = ziplistNew();
        zl = ziplistPush(zl,(unsigned char*)v1,strlen(v1),ZIPLIST_TAIL);
        zl = ziplistPush(zl,(unsigned char*)v2,strlen(v2),ZIPLIST_TAIL);

        /* Pop values again and compare their value. */
        p = ziplistIndex(zl,0);
        assert(ziplistGet(p,&entry,&elen,&value));
        assert(strncmp(v1,(char*)entry,elen) == 0);
        p = ziplistIndex(zl,1);
        assert(ziplistGet(p,&entry,&elen,&value));
        assert(strncmp(v2,(char*)entry,elen) == 0);
        printf("SUCCESS\n\n");
        zfree(zl);
    }

    printf("Regression test deleting next to last entries:\n");
    {
        char v[3][257] = {{0}};
        zlentry e[3] = {{.prevrawlensize = 0, .prevrawlen = 0, .lensize = 0,
                         .len = 0, .headersize = 0, .encoding = 0, .p = NULL}};
        size_t i;

        for (i = 0; i < (sizeof(v)/sizeof(v[0])); i++) {
            memset(v[i], 'a' + i, sizeof(v[0]));
        }

        v[0][256] = '\0';
        v[1][  1] = '\0';
        v[2][256] = '\0';

        zl = ziplistNew();
        for (i = 0; i < (sizeof(v)/sizeof(v[0])); i++) {
            zl = ziplistPush(zl, (unsigned char *) v[i], strlen(v[i]), ZIPLIST_TAIL);
        }

        verify(zl, e);

        assert(e[0].prevrawlensize == 1);
        assert(e[1].prevrawlensize == 5);
        assert(e[2].prevrawlensize == 1);

        /* Deleting entry 1 will increase `prevrawlensize` for entry 2 */
        unsigned char *p = e[1].p;
        zl = ziplistDelete(zl, &p);

        verify(zl, e);

        assert(e[0].prevrawlensize == 1);
        assert(e[1].prevrawlensize == 5);

        printf("SUCCESS\n\n");
        zfree(zl);
    }

    printf("Create long list and check indices:\n");
    {
        zl = ziplistNew();
        char buf[32];
        int i,len;
        for (i = 0; i < 1000; i++) {
            len = sprintf(buf,"%d",i);
            zl = ziplistPush(zl,(unsigned char*)buf,len,ZIPLIST_TAIL);
        }
        for (i = 0; i < 1000; i++) {
            p = ziplistIndex(zl,i);
            assert(ziplistGet(p,NULL,NULL,&value));
            assert(i == value);

            p = ziplistIndex(zl,-i-1);
            assert(ziplistGet(p,NULL,NULL,&value));
            assert(999-i == value);
        }
        printf("SUCCESS\n\n");
        zfree(zl);
    }

    printf("Compare strings with ziplist entries:\n");
    {
        zl = createList();
        p = ziplistIndex(zl,0);
        if (!ziplistCompare(p,(unsigned char*)"hello",5)) {
            printf("ERROR: not \"hello\"\n");
            return 1;
        }
        if (ziplistCompare(p,(unsigned char*)"hella",5)) {
            printf("ERROR: \"hella\"\n");
            return 1;
        }

        p = ziplistIndex(zl,3);
        if (!ziplistCompare(p,(unsigned char*)"1024",4)) {
            printf("ERROR: not \"1024\"\n");
            return 1;
        }
        if (ziplistCompare(p,(unsigned char*)"1025",4)) {
            printf("ERROR: \"1025\"\n");
            return 1;
        }
        printf("SUCCESS\n\n");
        zfree(zl);
    }

    printf("Merge test:\n");
    {
        /* create list gives us: [hello, foo, quux, 1024] */
        zl = createList();
        unsigned char *zl2 = createList();

        unsigned char *zl3 = ziplistNew();
        unsigned char *zl4 = ziplistNew();

        if (ziplistMerge(&zl4, &zl4)) {
            printf("ERROR: Allowed merging of one ziplist into itself.\n");
            return 1;
        }

        /* Merge two empty ziplists, get empty result back. */
        zl4 = ziplistMerge(&zl3, &zl4);
        ziplistRepr(zl4);
        if (ziplistLen(zl4)) {
            printf("ERROR: Merging two empty ziplists created entries.\n");
            return 1;
        }
        zfree(zl4);

        zl2 = ziplistMerge(&zl, &zl2);
        /* merge gives us: [hello, foo, quux, 1024, hello, foo, quux, 1024] */
        ziplistRepr(zl2);

        if (ziplistLen(zl2) != 8) {
            printf("ERROR: Merged length not 8, but: %u\n", ziplistLen(zl2));
            return 1;
        }

        p = ziplistIndex(zl2,0);
        if (!ziplistCompare(p,(unsigned char*)"hello",5)) {
            printf("ERROR: not \"hello\"\n");
            return 1;
        }
        if (ziplistCompare(p,(unsigned char*)"hella",5)) {
            printf("ERROR: \"hella\"\n");
            return 1;
        }

        p = ziplistIndex(zl2,3);
        if (!ziplistCompare(p,(unsigned char*)"1024",4)) {
            printf("ERROR: not \"1024\"\n");
            return 1;
        }
        if (ziplistCompare(p,(unsigned char*)"1025",4)) {
            printf("ERROR: \"1025\"\n");
            return 1;
        }

        p = ziplistIndex(zl2,4);
        if (!ziplistCompare(p,(unsigned char*)"hello",5)) {
            printf("ERROR: not \"hello\"\n");
            return 1;
        }
        if (ziplistCompare(p,(unsigned char*)"hella",5)) {
            printf("ERROR: \"hella\"\n");
            return 1;
        }

        p = ziplistIndex(zl2,7);
        if (!ziplistCompare(p,(unsigned char*)"1024",4)) {
            printf("ERROR: not \"1024\"\n");
            return 1;
        }
        if (ziplistCompare(p,(unsigned char*)"1025",4)) {
            printf("ERROR: \"1025\"\n");
            return 1;
        }
        printf("SUCCESS\n\n");
        zfree(zl);
    }

    printf("Stress with random payloads of different encoding:\n");
    {
        int i,j,len,where;
        unsigned char *p;
        char buf[1024];
        int buflen;
        list *ref;
        listNode *refnode;

        /* Hold temp vars from ziplist */
        unsigned char *sstr;
        unsigned int slen;
        long long sval;

        for (i = 0; i < 20000; i++) {
            zl = ziplistNew();
            ref = listCreate();
            listSetFreeMethod(ref,(void (*)(void*))sdsfree);
            len = rand() % 256;

            /* Create lists */
            for (j = 0; j < len; j++) {
                where = (rand() & 1) ? ZIPLIST_HEAD : ZIPLIST_TAIL;
                if (rand() % 2) {
                    buflen = randstring(buf,1,sizeof(buf)-1);
                } else {
                    switch(rand() % 3) {
                    case 0:
                        buflen = sprintf(buf,"%lld",(0LL + rand()) >> 20);
                        break;
                    case 1:
                        buflen = sprintf(buf,"%lld",(0LL + rand()));
                        break;
                    case 2:
                        buflen = sprintf(buf,"%lld",(0LL + rand()) << 20);
                        break;
                    default:
                        assert(NULL);
                    }
                }

                /* Add to ziplist */
                zl = ziplistPush(zl, (unsigned char*)buf, buflen, where);

                /* Add to reference list */
                if (where == ZIPLIST_HEAD) {
                    listAddNodeHead(ref,sdsnewlen(buf, buflen));
                } else if (where == ZIPLIST_TAIL) {
                    listAddNodeTail(ref,sdsnewlen(buf, buflen));
                } else {
                    assert(NULL);
                }
            }

            assert(listLength(ref) == ziplistLen(zl));
            for (j = 0; j < len; j++) {
                /* Naive way to get elements, but similar to the stresser
                 * executed from the Tcl test suite. */
                p = ziplistIndex(zl,j);
                refnode = listIndex(ref,j);

                assert(ziplistGet(p,&sstr,&slen,&sval));
                if (sstr == NULL) {
                    buflen = sprintf(buf,"%lld",sval);
                } else {
                    buflen = slen;
                    memcpy(buf,sstr,buflen);
                    buf[buflen] = '\0';
                }
                assert(memcmp(buf,listNodeValue(refnode),buflen) == 0);
            }
            zfree(zl);
            listRelease(ref);
        }
        printf("SUCCESS\n\n");
    }

    printf("Stress with variable ziplist size:\n");
    {
        stress(ZIPLIST_HEAD,100000,16384,256);
        stress(ZIPLIST_TAIL,100000,16384,256);
    }

    return 0;
}
#endif
