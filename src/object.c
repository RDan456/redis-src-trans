///redis object 的具体实现

///redis中Object的编码格式规定
///#define OBJ_ENCODING_RAW 0         原始表示
///#define OBJ_ENCODING_INT 1         表示为整数类型
///#define OBJ_ENCODING_HT 2          表示为字典类型
///#define OBJ_ENCODING_ZIPMAP 3      表示为压缩map
///#define OBJ_ENCODING_LINKEDLIST 4  表示为链表，这个没有在使用了
///#define OBJ_ENCODING_ZIPLIST 5     表示为压缩表类型
///#define OBJ_ENCODING_INTSET 6      表示为整数集合类型
///#define OBJ_ENCODING_SKIPLIST 7    表示为跳跃表类型
///#define OBJ_ENCODING_EMBSTR 8      表示为动态字符串类型
///#define OBJ_ENCODING_QUICKLIST 9   表示为快表类型
///#define OBJ_ENCODING_STREAM 10     /* Encoded as a radix tree of listpacks */

#include "server.h"
#include <math.h>
#include <ctype.h>

#ifdef __CYGWIN__
#define strtold(a,b) ((long double)strtod((a),(b)))
#endif

/* ===================== redis Object的创建和解析 ==================== */
///创建一个新的Object
robj *createObject(int type, void *ptr) { ///参数需要传入Object的类型和对应Object值的指针
    robj *o = zmalloc(sizeof(*o));        ///为这个对象申请内存空间
    o->type = type;                       ///对对象的type进行初始化
    o->encoding = OBJ_ENCODING_RAW;       ///对对象的编码格式进行初始化， 默认为原始类型
    o->ptr = ptr;                         ///为对象的值进行赋值操作
    o->refcount = 1;                      /// 设置对象的引用计数，初始化值为1

    /* Set the LRU to the current lruclock (minutes resolution), or
     * alternatively the LFU counter. */
    ///将LRU设置为当前lruclock 或者 LFU计数器。 
    if (server.maxmemory_policy & MAXMEMORY_FLAG_LFU) {
        o->lru = (LFUGetTimeInMinutes()<<8) | LFU_INIT_VAL;
    } else {
        o->lru = LRU_CLOCK();
    }
    return o;
}

/* Set a special refcount in the object to make it "shared":
 * incrRefCount and decrRefCount() will test for this special refcount
 * and will not touch the object. This way it is free to access shared
 * objects such as small integers from different threads without any
 * mutex.
 *
 * 常见的创建共享变量的方式为：
 *
 * robj *myobject = makeObjectShared(createObject(...));
 *
 */
///创建一个共享的变量，让它的引用计数为OBJ_SHARED_REFCOUNT（Int.MAX_VALE）
robj *makeObjectShared(robj *o) {

    serverAssert(o->refcount == 1);
    o->refcount = OBJ_SHARED_REFCOUNT;
    return o;
}

///创建一个编码为OBJ_ENCODING_RAW的字符串对象，这是一个纯字符串对象，其中o-> ptr指向正确的sds字符串。 
robj *createRawStringObject(const char *ptr, size_t len) {
    
	return createObject(OBJ_STRING, sdsnewlen(ptr,len));
}

///创建一个编码为OBJ_ENCODING_EMBSTR的字符串对象，该对象是sds字符串实际上是与该对象本身分配在同一块中的不可修改的字符串。 
robj *createEmbeddedStringObject(const char *ptr, size_t len) {

	///先分配所需要的内存空间，大小为对象大小以及sds字符串（head（sds头） + len + 1(结尾标识符)）需要占用的空间
    robj *o = zmalloc(sizeof(robj)+sizeof(struct sdshdr8)+len+1); 
    struct sdshdr8 *sh = (void*)(o+1); ///获取sds动态字符串的对象头地址

    o->type = OBJ_STRING; ///设置对象的type，为字符串类型
    o->encoding = OBJ_ENCODING_EMBSTR; ///设置对象的编码格式，为动态字符串
    o->ptr = sh+1; ///设置对象值的指针，具体指向动态字符串
    o->refcount = 1; ///设置对象的引用计数，大小为1
    if (server.maxmemory_policy & MAXMEMORY_FLAG_LFU) { ///设置对象的LUR相关的参数
        o->lru = (LFUGetTimeInMinutes()<<8) | LFU_INIT_VAL;
    } else {
        o->lru = LRU_CLOCK();
    }

	///下面设置动态字符串表头的相关参数
    sh->len = len; ///设置字符串的长度
    sh->alloc = len; ///设置字符串的申请内存长度
    sh->flags = SDS_TYPE_8; ///设置字符串的标识位
    if (ptr == SDS_NOINIT)  ///如果创建的字符串没有初始化
        sh->buf[len] = '\0'; ///在buf[]数组中设置结尾标识符
    else if (ptr) { ///如果需要构建的字符串不为空，
        memcpy(sh->buf,ptr,len); ///将需要构建的字符串拷贝到sh地址出
        sh->buf[len] = '\0'; ///设置字符串的结尾标识符
    } else {                 ///如果构建的是空的字符串，无需进行拷贝操作
        memset(sh->buf,0,len+1);  ///设置字符串的长度为0
    }
    return o; ///返回创建的对象
}

/*如果字符串对象小于OBJ_ENCODING_EMBSTR_SIZE_LIMIT，则使用EMBSTR编码（动态字符串编码）创建一个字符串对象，否则使用RAW编码（原始编码）。
 *选择当前限制为44，以便我们分配为EMBSTR的最大字符串对象仍然适合jemalloc的64字节区域。 
 */
#define OBJ_ENCODING_EMBSTR_SIZE_LIMIT 44  
robj *createStringObject(const char *ptr, size_t len) {

    if (len <= OBJ_ENCODING_EMBSTR_SIZE_LIMIT) ///如果要创建的字符串的len小于等于44
        return createEmbeddedStringObject(ptr,len);
    else                                       ///如果要创建的字符串的len大于44
        return createRawStringObject(ptr,len);
}

/* 从long long值创建一个字符串对象。 如果可能，返回一个共享的整数对象，或至少一个整数编码的对象。
 *
 * 如果valueobj不为零，则该函数避免返回一个共享整数，因为该对象将用作Redis键空间中的值
 *（例如，当使用INCR命令时），因此我们需要特定于LFU / LRU的值 每个键。 
 */
robj *createStringObjectFromLongLongWithOptions(long long value, int valueobj) {
    robj *o;

    if (server.maxmemory == 0 || 
        !(server.maxmemory_policy & MAXMEMORY_FLAG_NO_SHARED_INTEGERS)) 
    {
        ///如果maxmemory策略允许，即使valueobj为true，我们仍然可以返回共享整数。 
        valueobj = 0;
    }

	///如果value >= 0 并且 小于10000 并且valueObj == 0
    if (value >= 0 && value < OBJ_SHARED_INTEGERS && valueobj == 0) {
        
        incrRefCount(shared.integers[value]); ///增加对象的引用计数
        o = shared.integers[value]; ///设置对象
    } else { ///如果不满足上述的条件                      
        if (value >= LONG_MIN && value <= LONG_MAX) { ///value的值大于LONG_MIN并且小于LONG_MAX
            o = createObject(OBJ_STRING, NULL); ///创建一个字符串对象
            o->encoding = OBJ_ENCODING_INT;    ///将它的编码格式设置为整数编码
            o->ptr = (void*)((long)value);     ///设置对象的值
        } else { ///如果这个数据是long long类型
            o = createObject(OBJ_STRING,sdsfromlonglong(value)); ///将这个long long类型的数据转化为字符串
        }
    }
    return o; ///返回新创建的对象
}

/// 如果可能，createStringObjectFromLongLongWithOptions（）的包装始终要求创建一个共享对象。
robj *createStringObjectFromLongLong(long long value) {
    
    return createStringObjectFromLongLongWithOptions(value,0);
}

/* Wrapper for createStringObjectFromLongLongWithOptions() avoiding a shared
 * object when LFU/LRU info are needed, that is, when the object is used
 * as a value in the key space, and Redis is configured to evict based on
 * LFU/LRU. */
///createStringObjectFromLongLongWithOptions（）的包装器在需要LFU / LRU信息时（即，将对象用作键空间中的值时）
///避免了共享对象，并且Redis配置为基于LFU / LRU退出。
robj *createStringObjectFromLongLongForValue(long long value) {

    return createStringObjectFromLongLongWithOptions(value,1);
}

/* Create a string object from a long double. If humanfriendly is non-zero
 * it does not use exponential format and trims trailing zeroes at the end,
 * however this results in loss of precision. Otherwise exp format is used
 * and the output of snprintf() is not modified.
 *
 * The 'humanfriendly' option is used for INCRBYFLOAT and HINCRBYFLOAT. */
/// 用一个long double类型的数据构造一个字符串
robj *createStringObjectFromLongDouble(long double value, int humanfriendly) {

    char buf[MAX_LONG_DOUBLE_CHARS]; 
    int len = ld2string(buf,sizeof(buf),value,humanfriendly? LD_STR_HUMAN: LD_STR_AUTO);
    return createStringObject(buf,len);
}

/* 复制一个字符串对象，并确保返回的对象具有与原始对象相同的编码。
 * 此函数还保证复制小整数对象（或包含小整数表示形式的字符串对象）将始终导致不共享的新对象（refcount == 1）。
 * 结果对象始终将refcount设置为1。
 */
robj *dupStringObject(const robj *o) {
    
	robj *d;
    serverAssert(o->type == OBJ_STRING);

    switch(o->encoding) { ///通过o的编码格式来确保对象属于哪种类型
    case OBJ_ENCODING_RAW: ///如果是原始类型
        return createRawStringObject(o->ptr,sdslen(o->ptr)); 
    case OBJ_ENCODING_EMBSTR: ///如果是动态字符串类型
        return createEmbeddedStringObject(o->ptr,sdslen(o->ptr));
    case OBJ_ENCODING_INT: ///如果整数类型
        d = createObject(OBJ_STRING, NULL);
        d->encoding = OBJ_ENCODING_INT; ///设置对象的编码格式
        d->ptr = o->ptr; ///设置对象值的指针
        return d;
    default: ///如果是别的编码格式，表示错误的编码格式
        serverPanic("Wrong encoding."); 
        break;
    }
}

///创建对象，编码格式为quicklist（快表）
robj *createQuicklistObject(void) {
    
    quicklist *l = quicklistCreate(); ///首先需要新创建快表
    robj *o = createObject(OBJ_LIST,l); ///创建一个新的对象，他的ptr指向l。type为OBJ_LIST
    o->encoding = OBJ_ENCODING_QUICKLIST; ///设置对象的编码格式，为OBJ_ENCODING_QUICKLIST
    return o; 
}

///创建对象，编码格式为ziplist（压缩表）
robj *createZiplistObject(void) {

    unsigned char *zl = ziplistNew(); ///首先需要创建一个ziplist对象
    robj *o = createObject(OBJ_LIST,zl); ///创建一个新的对象，它的type为OBJ_LIST，ptr指向对象为zl
    o->encoding = OBJ_ENCODING_ZIPLIST; ///设置对象的编码格式，为OBJ_ENCODING_ZIPLIST
    return o;
}

///创建集合对象，编码格式为ht
robj *createSetObject(void) {

    dict *d = dictCreate(&setDictType,NULL); ///创建一个字典
    robj *o = createObject(OBJ_SET,d); ///创建一个新的对象，它的type为OBJ_SET，ptr指向对象为d
    o->encoding = OBJ_ENCODING_HT;///设置对象的编码格式，为OBJ_ENCODING_HT
    return o;
}


///创建集合对象，编码格式为insert（整数集合）
robj *createIntsetObject(void) {

    intset *is = intsetNew(); ///首先创建一个insert对象
    robj *o = createObject(OBJ_SET,is); ///创建一个新的对象，它的type为OBJ_SET，ptr指向对象为is
    o->encoding = OBJ_ENCODING_INTSET;  ///设置对象的编码格式，为OBJ_ENCODING_INTSET
    return o;
}

///创建hash对象， 编码格式为ziplist（压缩表）
robj *createHashObject(void) {

    unsigned char *zl = ziplistNew();///首先需要创建一个ziplist对象
    robj *o = createObject(OBJ_HASH, zl);///创建一个新的对象，它的type为OBJ_HASH，ptr指向对象为zl
    o->encoding = OBJ_ENCODING_ZIPLIST; ///设置对象的编码格式，为OBJ_ENCODING_ZIPLIST
    return o;
}

///创建一个有序集合对象，编码格式为skiplist(跳跃表)
robj *createZsetObject(void) {

    zset *zs = zmalloc(sizeof(*zs)); ///分配内存空间
    robj *o;

    zs->dict = dictCreate(&zsetDictType,NULL); ///创建并设置zs的字典指针
    zs->zsl = zslCreate(); ///创建病设置zs的
    o = createObject(OBJ_ZSET,zs); ///创建一个新的对象，它的type为OBJ_ZSET，ptr指向对象为zs
    o->encoding = OBJ_ENCODING_SKIPLIST; ///设置对象的编码格式，为OBJ_ENCODING_SKIPLIST
    return o;
}

///创建一个有序集合，编码格式为ziplist
robj *createZsetZiplistObject(void) { 

    unsigned char *zl = ziplistNew(); ///创建一个压缩表对象
    robj *o = createObject(OBJ_ZSET,zl); ///创建一个新的对象，它的type为OBJ_ZSET，ptr指向对象为zl
    o->encoding = OBJ_ENCODING_ZIPLIST;  ///设置对象的编码格式，为OBJ_ENCODING_ZIPLIST
    return o;
}

///创建一个流对象，编码格式为stream
robj *createStreamObject(void) {
   
	stream *s = streamNew(); ///创建一个流对象
    robj *o = createObject(OBJ_STREAM,s); ///创建一个新的对象type为OBJ_STREAM， ptr指向的对象为s
    o->encoding = OBJ_ENCODING_STREAM; ///设置对象的比那么格式，为OBJ_ENCODING_STREAM
    return o;
}

///创建一个Module对象
robj *createModuleObject(moduleType *mt, void *value) {
    
    moduleValue *mv = zmalloc(sizeof(*mv)); ///申请内存空间
    mv->type = mt; ///给type赋值
    mv->value = value; 给value赋值
    return createObject(OBJ_MODULE,mv); ///创建一个新的对象
}

///释放字符串对象
void freeStringObject(robj *o) {
    if (o->encoding == OBJ_ENCODING_RAW) { ///如果编码格式为原始格式，就直接释放字符串即可
        sdsfree(o->ptr);
    }
}

///释放list对象
void freeListObject(robj *o) {
    if (o->encoding == OBJ_ENCODING_QUICKLIST) { ///如果编码格式是quicklist
        quicklistRelease(o->ptr); ///释放o指向的快表
    } else { ///如果是别的编码格式，则直接打印异常
        serverPanic("Unknown list encoding type");
    }
}
 
///释放set集合对象
void freeSetObject(robj *o) {
    switch (o->encoding) {
    case OBJ_ENCODING_HT: ///如果编码格式为ht
        dictRelease((dict*) o->ptr); ///释放o的ptr指向的字典表
        break;
    case OBJ_ENCODING_INTSET: ///如果是整数集合（intset）编码
        zfree(o->ptr); ///直接释放o的ptr所指的内容
        break;
    default: ///其他的类型表示都存在问题
        serverPanic("Unknown set encoding type");
    }
}
 
///释放有序集合
void freeZsetObject(robj *o) {
    zset *zs;
    switch (o->encoding) {
    case OBJ_ENCODING_SKIPLIST: ///如果是skiplist（跳跃表）
        zs = o->ptr; ///获取o的ptr指向的内容
        dictRelease(zs->dict); ///释放zs指向的字典
        zslFree(zs->zsl); ///释放zs指向的zsl
        zfree(zs); ///释放o的ptr指向的内容
        break;
    case OBJ_ENCODING_ZIPLIST: ///如果是ziplist类型的编码
        zfree(o->ptr); ///直接释放o的ptr指向的内容
        break;
    default: ///如果是其他类型的编码，则打印异常
        serverPanic("Unknown sorted set encoding");
    }
}

///释放hash对象
void freeHashObject(robj *o) {
    switch (o->encoding) {
    case OBJ_ENCODING_HT: ///如果是ht类型的编码
        dictRelease((dict*) o->ptr); ///释放o的ptr指向的字典表
        break;
    case OBJ_ENCODING_ZIPLIST: ///如果是ziplist类型的编码
        zfree(o->ptr); ///释放o指向的ptr指针
        break;
    default: ///其他类型的编码则直接打印异常
        serverPanic("Unknown hash encoding type");
        break;
    }
}


///释放module类型对象
void freeModuleObject(robj *o) {
   
    moduleValue *mv = o->ptr; ///获取o的ptr指向的内容
    mv->type->free(mv->value); 
    zfree(mv);  ///释放mv
}

///释放stream类型对象
void freeStreamObject(robj *o) {

    freeStream(o->ptr); ///直接释放对象
}

///增加对象的引用计数
void incrRefCount(robj *o) {

    if (o->refcount < OBJ_FIRST_SPECIAL_REFCOUNT) { ///如果o的引用计数小于INT.MAX_VALUE - 1
        o->refcount++;  ///直接讲他的引用计数加1
    } else { 
        if (o->refcount == OBJ_SHARED_REFCOUNT) { ///如果等于OBJ_SHARED_REFCOUNT，是一个共享变量，什么都不需要做
            /* Nothing to do: this refcount is immutable. */
        } else if (o->refcount == OBJ_STATIC_REFCOUNT) { ///如果等于OBJ_STATIC_REFCOUNT，则打印异常
            serverPanic("You tried to retain an object allocated in the stack");
        }
    }
}

///减少对象的引用计数
void decrRefCount(robj *o) {
    if (o->refcount == 1) { ///如果对象的引用技术为1，-1操作之后变为0，表示这个对象不在被引用，需要将该对象进行释放
        switch(o->type) { ///通过o的type对对象进行释放
        case OBJ_STRING: freeStringObject(o); break;
        case OBJ_LIST: freeListObject(o); break;
        case OBJ_SET: freeSetObject(o); break;
        case OBJ_ZSET: freeZsetObject(o); break;
        case OBJ_HASH: freeHashObject(o); break;
        case OBJ_MODULE: freeModuleObject(o); break;
        case OBJ_STREAM: freeStreamObject(o); break;
        default: serverPanic("Unknown object type"); break;
        }
        zfree(o); ///最后释放o的内存空间
    } else {
        if (o->refcount <= 0) serverPanic("decrRefCount against refcount <= 0");
        ///如果o->refcount >1 ,就直接-1即可
		if (o->refcount != OBJ_SHARED_REFCOUNT) o->refcount--;
    }
}

///这个decrRefCount（）的变体将其参数声明为void，在希望该free方法的原型为“ void free_object（void *）”
///原型的数据结构中用作free方法很有用。
void decrRefCountVoid(void *o) {
    decrRefCount(o);
}

/* 此函数将引用计数设置为零而不释放对象。 为了将新对象传递给增加接收对象的引用计数的函数，这很有用。例如：
 *    functionThatWillIncrementRefCount(resetRefCount(CreateObject(...)));
 * 否则，您需要求助于不太优雅的模式：
 *    *obj = createObject(...);
 *    functionThatWillIncrementRefCount(obj);
 *    decrRefCount(obj);
 */
///重置引用计数
robj *resetRefCount(robj *obj) {
    obj->refcount = 0;
    return obj;
}

///对对象的type进行检测，1表示type有误，0表示正常
int checkType(client *c, robj *o, int type) {

    if (o->type != type) { ///
        addReply(c,shared.wrongtypeerr); ///给客户端回复类型错误的信息
        return 1;
    }
    return 0;
}

///判断字符串s是否可以转化为long long类型，如果行就将这个值写入到llval中，否则返回C_ERR
int isSdsRepresentableAsLongLong(sds s, long long *llval) {
    return string2ll(s,sdslen(s),llval) ? C_OK : C_ERR;
}

///判断一个对象o是否可以用long long来表示，如果能，将值保存到llval中，否则返回C_ERR
int isObjectRepresentableAsLongLong(robj *o, long long *llval) {

    serverAssertWithInfo(NULL,o,o->type == OBJ_STRING);
    if (o->encoding == OBJ_ENCODING_INT) { 如果是int类型的编码，表示是可以的
        if (llval) *llval = (long) o->ptr; ///强制类型转化
        return C_OK;
    } else { ///表示是字符串类型，调用上一个方法进行转化
        return isSdsRepresentableAsLongLong(o->ptr,llval);
    }
}

///如果在SDS字符串末尾有超过10％的可用空间，则优化字符串对象内部的SDS字符串以使其占用很少的空间。 
///发生这种情况是因为SDS字符串趋向于整体化以避免在附加到字符串时浪费过多的分配时间。 
void trimStringObjectIfNeeded(robj *o) {
    if (o->encoding == OBJ_ENCODING_RAW &&
        sdsavail(o->ptr) > sdslen(o->ptr)/10)
    {
        o->ptr = sdsRemoveFreeSpace(o->ptr); ///移除字符串中空闲空间
    }
}

/// 尝试对字符串对象进行编码以节省空间
robj *tryObjectEncoding(robj *o) {

    long value;
    sds s = o->ptr; ///获取字符串
    size_t len;

    ///确保这是一个字符串对象，这是我们在此函数中编码的唯一类型。 其他类型使用编码的内存有效表示形式，但由实现该类型的命令处理。
    serverAssertWithInfo(NULL,o,o->type == OBJ_STRING);

    ///我们仅对RAW或EMBSTR编码的对象尝试某些专门的编码，换句话说，仍然由实际的char数组表示的对象。
    if (!sdsEncodedObject(o)) return o;

    ///对共享库进行编码并不安全：共享库可以在Redis的“对象空间”中随处共享，并且可能会在未处理的地方终止。 我们仅将它们作为键空间中的值进行处理。 
     if (o->refcount > 1) return o;

    ///检查我们是否可以将此字符串表示为长整数。 请注意，我们确定大于20个字符的字符串不能表示为32位或64位整数。 
    len = sdslen(s);
    if (len <= 20 && string2l(s,len,&value)) {
        ///此对象可编码为long。尝试使用共享对象。请注意，在使用maxmemory时，我们避免使用共享整数，因为每个对象都需要具有私有
        ///LRU字段才能使LRU算法正常工作。 
        if ((server.maxmemory == 0 ||
            !(server.maxmemory_policy & MAXMEMORY_FLAG_NO_SHARED_INTEGERS)) &&
            value >= 0 &&
            value < OBJ_SHARED_INTEGERS) ///如果value处于共享整数的范围内
        {
            decrRefCount(o); ///修改对象的引用计数 -1
            incrRefCount(shared.integers[value]); ///共享变量的引用计数 +1
            return shared.integers[value];  //返回一个编码为整数的字符串对象
        } else {
            if (o->encoding == OBJ_ENCODING_RAW) { ///如果编码格式为原型
                sdsfree(o->ptr); ///释放o的ptr所指向的内容
                o->encoding = OBJ_ENCODING_INT; ///设置对象的编码格式为INT
                o->ptr = (void*) value; ///设置对象对象的值
                return o; 
            } else if (o->encoding == OBJ_ENCODING_EMBSTR) { ///如果是动态字符串编码
                decrRefCount(o); ///引用计数-1
                return createStringObjectFromLongLongForValue(value); ///将其转化为long long对象
            }
        }
    }

    ///如果字符串较小并且仍是RAW编码，请尝试更有效的EMBSTR编码。在这种表示形式中，对象和SDS字符串分配在同一块内存中，
    ///以节省空间和缓存未命中。 
    if (len <= OBJ_ENCODING_EMBSTR_SIZE_LIMIT) {
        robj *emb;

        if (o->encoding == OBJ_ENCODING_EMBSTR) return o;  ///将RAW对象转换为OBJ_ENCODING_EMBSTR编码类型
        emb = createEmbeddedStringObject(s,sdslen(s)); ///创建一个编码格式为OBJ_ENCODING_EMBSTR的字符串
        decrRefCount(o); ///对原对象的引用计数-1
        return emb;
    }

    /* 我们无法编码对象...
     * 进行最后一次尝试，并至少优化字符串对象内的SDS字符串以要求很少的空间，以防SDS字符串末尾的可用空间超过10％。
     * 我们仅对相对较大的字符串执行此操作，因为仅当字符串的长度大于OBJ_ENCODING_EMBSTR_SIZE_LIMIT时才输入此分支。
     */
    trimStringObjectIfNeeded(o);

    ///返回原来的对象
    return o;
}

///获取编码对象的解码版本（作为新对象返回）。如果该对象已经过原始编码，则只需增加引用计数即可。
robj *getDecodedObject(robj *o) {
    robj *dec;

    if (sdsEncodedObject(o)) { ///字符串类型的对象解码
        incrRefCount(o); ///增加对象的引用计数
        return o;
    }
    if (o->type == OBJ_STRING && o->encoding == OBJ_ENCODING_INT) { ///如果o的类型为stirng，并且编码格式为int
        char buf[32];

        ll2string(buf,32,(long)o->ptr); ///将这个整数转化为字符串
        dec = createStringObject(buf,strlen(buf)); ///新建一个字符串
        return dec; 
    } else {
        serverPanic("Unknown encoding type");
    }
}

/* 根据标志，通过strcmp()或strcoll()比较两个字符串对象。
 * 请注意，对象可能是整数编码的。在这种情况下，我们使用ll2string()获取堆栈上数字的字符串表示形式并比较字符串，
 * 这比调用getDecodedObject()快得多。
 * 重要说明：使用REDIS_COMPARE_BINARY时，二进制安全比较用来。 
 */

#define REDIS_COMPARE_BINARY (1<<0) ///二进制的方式比较，标识位为1
#define REDIS_COMPARE_COLL (1<<1)  ///通过字符次序进行比价，标识位为2

///比较两个字符串a,b比较的方式通过flag确定，相同返回0，否则返回非0整数
int compareStringObjectsWithFlags(robj *a, robj *b, int flags) {

    serverAssertWithInfo(NULL,a,a->type == OBJ_STRING && b->type == OBJ_STRING); ///要求a，b都为字符串类型
    char bufa[128], bufb[128], *astr, *bstr; 
    size_t alen, blen, minlen;

    if (a == b) return 0; ///如果a，b指向同一个地址，肯定是相同的字符串，直接返回
    if (sdsEncodedObject(a)) { ///如果指向的字符串是原始编码类型（RAW）类型或者动态字符串类型（EMBSTR）
        astr = a->ptr; ///获取字符串的值
        alen = sdslen(astr); ///设置字符串的长度
    } else {
        alen = ll2string(bufa,sizeof(bufa),(long) a->ptr); ///如果是数字，将其转化为字符串类型
        astr = bufa; ///设置这个字符串的长度
    }
    if (sdsEncodedObject(b)) { ///如果指向的字符串是原始编码类型（RAW）类型或者动态字符串类型（EMBSTR）
        bstr = b->ptr; ///获取字符串的值
        blen = sdslen(bstr); ///设置字符串的长度
    } else {
        blen = ll2string(bufb,sizeof(bufb),(long) b->ptr);///如果是数字，将其转化为字符串类型
        bstr = bufb;  ///设置这个字符串的长度
    }
    if (flags & REDIS_COMPARE_COLL) { ///如果是本地字符串比较
        return strcoll(astr,bstr); ///调用strcoll()函数进行比较
    } else { ///如果是二进制的方式比较
        int cmp;

        minlen = (alen < blen) ? alen : blen; ///获取较短字符串的长度
        cmp = memcmp(astr,bstr,minlen); ///比较两个字符串是否相等 
        if (cmp == 0) return alen-blen; ///如果不相等，返回两个字符串的长度差
        return cmp;
    }
}

///使用二进制比较的compareStringObjectsWithFlags（）的包装器。
int compareStringObjects(robj *a, robj *b) {
    return compareStringObjectsWithFlags(a,b,REDIS_COMPARE_BINARY);
}

///使用排序规则的compareStringObjectsWithFlags（）的包装器。 
int collateStringObjects(robj *a, robj *b) {
    return compareStringObjectsWithFlags(a,b,REDIS_COMPARE_COLL);
}

///从字符串比较的角度来看，如果两个对象相同，则相等的字符串对象返回1，否则返回0。请注意，此函数比检查
///（compareStringObject（a，b）== 0）更快，因为它可以执行更多优化。 
int equalStringObjects(robj *a, robj *b) {

	///如果a,b都是整数编码格式，只需要比较这两个对象的值是否相等即可
    if (a->encoding == OBJ_ENCODING_INT && b->encoding == OBJ_ENCODING_INT){
       
        return a->ptr == b->ptr;
    } else { ///如果不是整数类型编码，需要调用compareStringObjects()方法比较两个对象
        return compareStringObjects(a,b) == 0;
    }
}

///获取字符串对象的长度
size_t stringObjectLen(robj *o) {

    serverAssertWithInfo(NULL,o,o->type == OBJ_STRING);///判定对象类型为字符串
    if (sdsEncodedObject(o)) { ///如果o是原始编码（RAW）或者动态字符串编码（EMBSTR）
        return sdslen(o->ptr); ///返回字符串的长度
    } else { 
        return sdigits10((long)o->ptr); ///如果是整数编码，计算出整数的位数，并返回
    }
}

///从字符串对象中获取double数据，并将这个值保存到target中
int getDoubleFromObject(const robj *o, double *target) {
    double value;

    if (o == NULL) { ///如果是空对象，让value = 0 
        value = 0;
    } else {
        serverAssertWithInfo(NULL,o,o->type == OBJ_STRING); ///判断对象类型，需要为字符串类型
        if (sdsEncodedObject(o)) { ///如果o是原始编码（RAW）或者动态字符串编码（EMBSTR）
            if (!string2d(o->ptr, sdslen(o->ptr), &value)) ///如果字符串不能转化为都变了，直接返回C_ERR
                return C_ERR;
        } else if (o->encoding == OBJ_ENCODING_INT) { ///如果是整数类型编码，直接将这个字符串转化为double类型
            value = (long)o->ptr;
        } else {
            serverPanic("Unknown string encoding");
        }
    }
    *target = value; ///将value赋值给target，并且返回获取数据成功
    return C_OK;
}

///从对象o中获取double值，如果获取值成功，讲这个值保存在target中，并返回给客户端，如果失败，返回失败信息给客户端
int getDoubleFromObjectOrReply(client *c, robj *o, double *target, const char *msg) {
    double value;
    if (getDoubleFromObject(o, &value) != C_OK) { ///通过上面的方法获取double值，如果失败
        if (msg != NULL) { 
            addReplyError(c,(char*)msg); ///返回操作失败的信息给客户端
        } else {
            addReplyError(c,"value is not a valid float");
        }
        return C_ERR; ///返回失败
    }
    *target = value; ///获取值成功，讲value的值赋值给target
    return C_OK; ///返回操作成功
}

///从对象o获取long double类型的值，如果获取成功返回1，否则返回0，获取成狗后讲这个值保存到target中
int getLongDoubleFromObject(robj *o, long double *target) {
    long double value;
 
    if (o == NULL) {  ///如果是空对象，让value = 0 
        value = 0;
    } else {
        serverAssertWithInfo(NULL,o,o->type == OBJ_STRING);  ///判断对象类型，需要为字符串类型
        if (sdsEncodedObject(o)) {  ///如果o是原始编码（RAW）或者动态字符串编码（EMBSTR）
            if (!string2ld(o->ptr, sdslen(o->ptr), &value)) ///如果字符串不能转化为都变了，直接返回C_ERR
                return C_ERR;
        } else if (o->encoding == OBJ_ENCODING_INT) { ///如果是整数类型编码，直接将这个字符串转化为double类型
            value = (long)o->ptr;
        } else {
            serverPanic("Unknown string encoding");
        }
    }
    *target = value; ///将value赋值给target，并且返回获取数据成功
    return C_OK;
}

///从对象o中获取 long double值，如果获取值成功，讲这个值保存在target中，并返回给客户端，如果失败，返回失败信息给客户端
int getLongDoubleFromObjectOrReply(client *c, robj *o, long double *target, const char *msg) {
    long double value;
    if (getLongDoubleFromObject(o, &value) != C_OK) {
        if (msg != NULL) {
            addReplyError(c,(char*)msg);
        } else {
            addReplyError(c,"value is not a valid float");
        }
        return C_ERR;
    }
    *target = value;
    return C_OK;
}

///从对象o获取long long类型的值，如果获取成功返回1，否则返回0，获取成狗后讲这个值保存到target中
int getLongLongFromObject(robj *o, long long *target) {
    long long value;

    if (o == NULL) {
        value = 0;
    } else {
        serverAssertWithInfo(NULL,o,o->type == OBJ_STRING);
        if (sdsEncodedObject(o)) {
            if (string2ll(o->ptr,sdslen(o->ptr),&value) == 0) return C_ERR;
        } else if (o->encoding == OBJ_ENCODING_INT) {
            value = (long)o->ptr;
        } else {
            serverPanic("Unknown string encoding");
        }
    }
    if (target) *target = value;
    return C_OK;
}

///从对象o中获取 long long值，如果获取值成功，讲这个值保存在target中，并返回给客户端，如果失败，返回失败信息给客户端
int getLongLongFromObjectOrReply(client *c, robj *o, long long *target, const char *msg) {
    long long value;
    if (getLongLongFromObject(o, &value) != C_OK) {
        if (msg != NULL) {
            addReplyError(c,(char*)msg);
        } else {
            addReplyError(c,"value is not an integer or out of range");
        }
        return C_ERR;
    }
    *target = value;
    return C_OK;
}

///从对象o中获取long值，如果获取值成功，讲这个值保存在target中，并返回给客户端，如果失败，返回失败信息给客户端
int getLongFromObjectOrReply(client *c, robj *o, long *target, const char *msg) {
    long long value;

    if (getLongLongFromObjectOrReply(c, o, &value, msg) != C_OK) return C_ERR;
    if (value < LONG_MIN || value > LONG_MAX) {
        if (msg != NULL) {
            addReplyError(c,(char*)msg);
        } else {
            addReplyError(c,"value is out of range");
        }
        return C_ERR;
    }
    *target = value;
    return C_OK;
}

///获取编码值对应的编码名称
char *strEncoding(int encoding) {
    
	switch(encoding) {
    case OBJ_ENCODING_RAW: return "raw"; ///原始类型编码
    case OBJ_ENCODING_INT: return "int"; ///整数类型编码
    case OBJ_ENCODING_HT: return "hashtable"; ///字典类型编码
    case OBJ_ENCODING_QUICKLIST: return "quicklist"; ///快表类型编码
    case OBJ_ENCODING_ZIPLIST: return "ziplist"; ///压缩表类型编码
    case OBJ_ENCODING_INTSET: return "intset"; ///整数集合类型编码
    case OBJ_ENCODING_SKIPLIST: return "skiplist"; ///跳跃表类型编码
    case OBJ_ENCODING_EMBSTR: return "embstr"; ///动态字符串类型编码
    default: return "unknown"; ///如果不是上面类型的编码，那么这种编码就是错误的
    }
} 

/* =========================== Memory introspection ========================= */
/*
 *下面的方法是关于内存的管理的方法，我先将redis的基本内容讲解完，然后再来讲这些
 */


/* This is an helper function with the goal of estimating the memory
 * size of a radix tree that is used to store Stream IDs.
 *
 * Note: to guess the size of the radix tree is not trivial, so we
 * approximate it considering 16 bytes of data overhead for each
 * key (the ID), and then adding the number of bare nodes, plus some
 * overhead due by the data and child pointers. This secret recipe
 * was obtained by checking the average radix tree created by real
 * workloads, and then adjusting the constants to get numbers that
 * more or less match the real memory usage.
 *
 * Actually the number of nodes and keys may be different depending
 * on the insertion speed and thus the ability of the radix tree
 * to compress prefixes. */
size_t streamRadixTreeMemoryUsage(rax *rax) {
    size_t size;
    size = rax->numele * sizeof(streamID);
    size += rax->numnodes * sizeof(raxNode);
    /* Add a fixed overhead due to the aux data pointer, children, ... */
    size += rax->numnodes * sizeof(long)*30;
    return size;
}

/* Returns the size in bytes consumed by the key's value in RAM.
 * Note that the returned value is just an approximation, especially in the
 * case of aggregated data types where only "sample_size" elements
 * are checked and averaged to estimate the total size. */
#define OBJ_COMPUTE_SIZE_DEF_SAMPLES 5 /* Default sample size. */
size_t objectComputeSize(robj *o, size_t sample_size) {
    sds ele, ele2;
    dict *d;
    dictIterator *di;
    struct dictEntry *de;
    size_t asize = 0, elesize = 0, samples = 0;

    if (o->type == OBJ_STRING) {
        if(o->encoding == OBJ_ENCODING_INT) {
            asize = sizeof(*o);
        } else if(o->encoding == OBJ_ENCODING_RAW) {
            asize = sdsAllocSize(o->ptr)+sizeof(*o);
        } else if(o->encoding == OBJ_ENCODING_EMBSTR) {
            asize = sdslen(o->ptr)+2+sizeof(*o);
        } else {
            serverPanic("Unknown string encoding");
        }
    } else if (o->type == OBJ_LIST) {
        if (o->encoding == OBJ_ENCODING_QUICKLIST) {
            quicklist *ql = o->ptr;
            quicklistNode *node = ql->head;
            asize = sizeof(*o)+sizeof(quicklist);
            do {
                elesize += sizeof(quicklistNode)+ziplistBlobLen(node->zl);
                samples++;
            } while ((node = node->next) && samples < sample_size);
            asize += (double)elesize/samples*ql->len;
        } else if (o->encoding == OBJ_ENCODING_ZIPLIST) {
            asize = sizeof(*o)+ziplistBlobLen(o->ptr);
        } else {
            serverPanic("Unknown list encoding");
        }
    } else if (o->type == OBJ_SET) {
        if (o->encoding == OBJ_ENCODING_HT) {
            d = o->ptr;
            di = dictGetIterator(d);
            asize = sizeof(*o)+sizeof(dict)+(sizeof(struct dictEntry*)*dictSlots(d));
            while((de = dictNext(di)) != NULL && samples < sample_size) {
                ele = dictGetKey(de);
                elesize += sizeof(struct dictEntry) + sdsAllocSize(ele);
                samples++;
            }
            dictReleaseIterator(di);
            if (samples) asize += (double)elesize/samples*dictSize(d);
        } else if (o->encoding == OBJ_ENCODING_INTSET) {
            intset *is = o->ptr;
            asize = sizeof(*o)+sizeof(*is)+is->encoding*is->length;
        } else {
            serverPanic("Unknown set encoding");
        }
    } else if (o->type == OBJ_ZSET) {
        if (o->encoding == OBJ_ENCODING_ZIPLIST) {
            asize = sizeof(*o)+(ziplistBlobLen(o->ptr));
        } else if (o->encoding == OBJ_ENCODING_SKIPLIST) {
            d = ((zset*)o->ptr)->dict;
            zskiplist *zsl = ((zset*)o->ptr)->zsl;
            zskiplistNode *znode = zsl->header->level[0].forward;
            asize = sizeof(*o)+sizeof(zset)+sizeof(zskiplist)+sizeof(dict)+
                    (sizeof(struct dictEntry*)*dictSlots(d))+
                    zmalloc_size(zsl->header);
            while(znode != NULL && samples < sample_size) {
                elesize += sdsAllocSize(znode->ele);
                elesize += sizeof(struct dictEntry) + zmalloc_size(znode);
                samples++;
                znode = znode->level[0].forward;
            }
            if (samples) asize += (double)elesize/samples*dictSize(d);
        } else {
            serverPanic("Unknown sorted set encoding");
        }
    } else if (o->type == OBJ_HASH) {
        if (o->encoding == OBJ_ENCODING_ZIPLIST) {
            asize = sizeof(*o)+(ziplistBlobLen(o->ptr));
        } else if (o->encoding == OBJ_ENCODING_HT) {
            d = o->ptr;
            di = dictGetIterator(d);
            asize = sizeof(*o)+sizeof(dict)+(sizeof(struct dictEntry*)*dictSlots(d));
            while((de = dictNext(di)) != NULL && samples < sample_size) {
                ele = dictGetKey(de);
                ele2 = dictGetVal(de);
                elesize += sdsAllocSize(ele) + sdsAllocSize(ele2);
                elesize += sizeof(struct dictEntry);
                samples++;
            }
            dictReleaseIterator(di);
            if (samples) asize += (double)elesize/samples*dictSize(d);
        } else {
            serverPanic("Unknown hash encoding");
        }
    } else if (o->type == OBJ_STREAM) {
        stream *s = o->ptr;
        asize = sizeof(*o);
        asize += streamRadixTreeMemoryUsage(s->rax);

        /* Now we have to add the listpacks. The last listpack is often non
         * complete, so we estimate the size of the first N listpacks, and
         * use the average to compute the size of the first N-1 listpacks, and
         * finally add the real size of the last node. */
        raxIterator ri;
        raxStart(&ri,s->rax);
        raxSeek(&ri,"^",NULL,0);
        size_t lpsize = 0, samples = 0;
        while(samples < sample_size && raxNext(&ri)) {
            unsigned char *lp = ri.data;
            lpsize += lpBytes(lp);
            samples++;
        }
        if (s->rax->numele <= samples) {
            asize += lpsize;
        } else {
            if (samples) lpsize /= samples; /* Compute the average. */
            asize += lpsize * (s->rax->numele-1);
            /* No need to check if seek succeeded, we enter this branch only
             * if there are a few elements in the radix tree. */
            raxSeek(&ri,"$",NULL,0);
            raxNext(&ri);
            asize += lpBytes(ri.data);
        }
        raxStop(&ri);

        /* Consumer groups also have a non trivial memory overhead if there
         * are many consumers and many groups, let's count at least the
         * overhead of the pending entries in the groups and consumers
         * PELs. */
        if (s->cgroups) {
            raxStart(&ri,s->cgroups);
            raxSeek(&ri,"^",NULL,0);
            while(raxNext(&ri)) {
                streamCG *cg = ri.data;
                asize += sizeof(*cg);
                asize += streamRadixTreeMemoryUsage(cg->pel);
                asize += sizeof(streamNACK)*raxSize(cg->pel);

                /* For each consumer we also need to add the basic data
                 * structures and the PEL memory usage. */
                raxIterator cri;
                raxStart(&cri,cg->consumers);
                raxSeek(&cri,"^",NULL,0);
                while(raxNext(&cri)) {
                    streamConsumer *consumer = cri.data;
                    asize += sizeof(*consumer);
                    asize += sdslen(consumer->name);
                    asize += streamRadixTreeMemoryUsage(consumer->pel);
                    /* Don't count NACKs again, they are shared with the
                     * consumer group PEL. */
                }
                raxStop(&cri);
            }
            raxStop(&ri);
        }
    } else if (o->type == OBJ_MODULE) {
        moduleValue *mv = o->ptr;
        moduleType *mt = mv->type;
        if (mt->mem_usage != NULL) {
            asize = mt->mem_usage(mv->value);
        } else {
            asize = 0;
        }
    } else {
        serverPanic("Unknown object type");
    }
    return asize;
}

/* Release data obtained with getMemoryOverheadData(). */
void freeMemoryOverheadData(struct redisMemOverhead *mh) {
    zfree(mh->db);
    zfree(mh);
}

/* Return a struct redisMemOverhead filled with memory overhead
 * information used for the MEMORY OVERHEAD and INFO command. The returned
 * structure pointer should be freed calling freeMemoryOverheadData(). */
struct redisMemOverhead *getMemoryOverheadData(void) {
    int j;
    size_t mem_total = 0;
    size_t mem = 0;
    size_t zmalloc_used = zmalloc_used_memory();
    struct redisMemOverhead *mh = zcalloc(sizeof(*mh));

    mh->total_allocated = zmalloc_used;
    mh->startup_allocated = server.initial_memory_usage;
    mh->peak_allocated = server.stat_peak_memory;
    mh->total_frag =
        (float)server.cron_malloc_stats.process_rss / server.cron_malloc_stats.zmalloc_used;
    mh->total_frag_bytes =
        server.cron_malloc_stats.process_rss - server.cron_malloc_stats.zmalloc_used;
    mh->allocator_frag =
        (float)server.cron_malloc_stats.allocator_active / server.cron_malloc_stats.allocator_allocated;
    mh->allocator_frag_bytes =
        server.cron_malloc_stats.allocator_active - server.cron_malloc_stats.allocator_allocated;
    mh->allocator_rss =
        (float)server.cron_malloc_stats.allocator_resident / server.cron_malloc_stats.allocator_active;
    mh->allocator_rss_bytes =
        server.cron_malloc_stats.allocator_resident - server.cron_malloc_stats.allocator_active;
    mh->rss_extra =
        (float)server.cron_malloc_stats.process_rss / server.cron_malloc_stats.allocator_resident;
    mh->rss_extra_bytes =
        server.cron_malloc_stats.process_rss - server.cron_malloc_stats.allocator_resident;

    mem_total += server.initial_memory_usage;

    mem = 0;
    if (server.repl_backlog)
        mem += zmalloc_size(server.repl_backlog);
    mh->repl_backlog = mem;
    mem_total += mem;

    /* Computing the memory used by the clients would be O(N) if done
     * here online. We use our values computed incrementally by
     * clientsCronTrackClientsMemUsage(). */
    mh->clients_slaves = server.stat_clients_type_memory[CLIENT_TYPE_SLAVE];
    mh->clients_normal = server.stat_clients_type_memory[CLIENT_TYPE_MASTER]+
                         server.stat_clients_type_memory[CLIENT_TYPE_PUBSUB]+
                         server.stat_clients_type_memory[CLIENT_TYPE_NORMAL];
    mem_total += mh->clients_slaves;
    mem_total += mh->clients_normal;

    mem = 0;
    if (server.aof_state != AOF_OFF) {
        mem += sdsalloc(server.aof_buf);
        mem += aofRewriteBufferSize();
    }
    mh->aof_buffer = mem;
    mem_total+=mem;

    mem = server.lua_scripts_mem;
    mem += dictSize(server.lua_scripts) * sizeof(dictEntry) +
        dictSlots(server.lua_scripts) * sizeof(dictEntry*);
    mem += dictSize(server.repl_scriptcache_dict) * sizeof(dictEntry) +
        dictSlots(server.repl_scriptcache_dict) * sizeof(dictEntry*);
    if (listLength(server.repl_scriptcache_fifo) > 0) {
        mem += listLength(server.repl_scriptcache_fifo) * (sizeof(listNode) + 
            sdsZmallocSize(listNodeValue(listFirst(server.repl_scriptcache_fifo))));
    }
    mh->lua_caches = mem;
    mem_total+=mem;

    for (j = 0; j < server.dbnum; j++) {
        redisDb *db = server.db+j;
        long long keyscount = dictSize(db->dict);
        if (keyscount==0) continue;

        mh->total_keys += keyscount;
        mh->db = zrealloc(mh->db,sizeof(mh->db[0])*(mh->num_dbs+1));
        mh->db[mh->num_dbs].dbid = j;

        mem = dictSize(db->dict) * sizeof(dictEntry) +
              dictSlots(db->dict) * sizeof(dictEntry*) +
              dictSize(db->dict) * sizeof(robj);
        mh->db[mh->num_dbs].overhead_ht_main = mem;
        mem_total+=mem;

        mem = dictSize(db->expires) * sizeof(dictEntry) +
              dictSlots(db->expires) * sizeof(dictEntry*);
        mh->db[mh->num_dbs].overhead_ht_expires = mem;
        mem_total+=mem;

        mh->num_dbs++;
    }

    mh->overhead_total = mem_total;
    mh->dataset = zmalloc_used - mem_total;
    mh->peak_perc = (float)zmalloc_used*100/mh->peak_allocated;

    /* Metrics computed after subtracting the startup memory from
     * the total memory. */
    size_t net_usage = 1;
    if (zmalloc_used > mh->startup_allocated)
        net_usage = zmalloc_used - mh->startup_allocated;
    mh->dataset_perc = (float)mh->dataset*100/net_usage;
    mh->bytes_per_key = mh->total_keys ? (net_usage / mh->total_keys) : 0;

    return mh;
}

/* Helper for "MEMORY allocator-stats", used as a callback for the jemalloc
 * stats output. */
void inputCatSds(void *result, const char *str) {
    /* result is actually a (sds *), so re-cast it here */
    sds *info = (sds *)result;
    *info = sdscat(*info, str);
}

/* This implements MEMORY DOCTOR. An human readable analysis of the Redis
 * memory condition. */
sds getMemoryDoctorReport(void) {
    int empty = 0;          /* Instance is empty or almost empty. */
    int big_peak = 0;       /* Memory peak is much larger than used mem. */
    int high_frag = 0;      /* High fragmentation. */
    int high_alloc_frag = 0;/* High allocator fragmentation. */
    int high_proc_rss = 0;  /* High process rss overhead. */
    int high_alloc_rss = 0; /* High rss overhead. */
    int big_slave_buf = 0;  /* Slave buffers are too big. */
    int big_client_buf = 0; /* Client buffers are too big. */
    int many_scripts = 0;   /* Script cache has too many scripts. */
    int num_reports = 0;
    struct redisMemOverhead *mh = getMemoryOverheadData();

    if (mh->total_allocated < (1024*1024*5)) {
        empty = 1;
        num_reports++;
    } else {
        /* Peak is > 150% of current used memory? */
        if (((float)mh->peak_allocated / mh->total_allocated) > 1.5) {
            big_peak = 1;
            num_reports++;
        }

        /* Fragmentation is higher than 1.4 and 10MB ?*/
        if (mh->total_frag > 1.4 && mh->total_frag_bytes > 10<<20) {
            high_frag = 1;
            num_reports++;
        }

        /* External fragmentation is higher than 1.1 and 10MB? */
        if (mh->allocator_frag > 1.1 && mh->allocator_frag_bytes > 10<<20) {
            high_alloc_frag = 1;
            num_reports++;
        }

        /* Allocator rss is higher than 1.1 and 10MB ? */
        if (mh->allocator_rss > 1.1 && mh->allocator_rss_bytes > 10<<20) {
            high_alloc_rss = 1;
            num_reports++;
        }

        /* Non-Allocator rss is higher than 1.1 and 10MB ? */
        if (mh->rss_extra > 1.1 && mh->rss_extra_bytes > 10<<20) {
            high_proc_rss = 1;
            num_reports++;
        }

        /* Clients using more than 200k each average? */
        long numslaves = listLength(server.slaves);
        long numclients = listLength(server.clients)-numslaves;
        if (mh->clients_normal / numclients > (1024*200)) {
            big_client_buf = 1;
            num_reports++;
        }

        /* Slaves using more than 10 MB each? */
        if (numslaves > 0 && mh->clients_slaves / numslaves > (1024*1024*10)) {
            big_slave_buf = 1;
            num_reports++;
        }

        /* Too many scripts are cached? */
        if (dictSize(server.lua_scripts) > 1000) {
            many_scripts = 1;
            num_reports++;
        }
    }

    sds s;
    if (num_reports == 0) {
        s = sdsnew(
        "Hi Sam, I can't find any memory issue in your instance. "
        "I can only account for what occurs on this base.\n");
    } else if (empty == 1) {
        s = sdsnew(
        "Hi Sam, this instance is empty or is using very little memory, "
        "my issues detector can't be used in these conditions. "
        "Please, leave for your mission on Earth and fill it with some data. "
        "The new Sam and I will be back to our programming as soon as I "
        "finished rebooting.\n");
    } else {
        s = sdsnew("Sam, I detected a few issues in this Redis instance memory implants:\n\n");
        if (big_peak) {
            s = sdscat(s," * Peak memory: In the past this instance used more than 150% the memory that is currently using. The allocator is normally not able to release memory after a peak, so you can expect to see a big fragmentation ratio, however this is actually harmless and is only due to the memory peak, and if the Redis instance Resident Set Size (RSS) is currently bigger than expected, the memory will be used as soon as you fill the Redis instance with more data. If the memory peak was only occasional and you want to try to reclaim memory, please try the MEMORY PURGE command, otherwise the only other option is to shutdown and restart the instance.\n\n");
        }
        if (high_frag) {
            s = sdscatprintf(s," * High total RSS: This instance has a memory fragmentation and RSS overhead greater than 1.4 (this means that the Resident Set Size of the Redis process is much larger than the sum of the logical allocations Redis performed). This problem is usually due either to a large peak memory (check if there is a peak memory entry above in the report) or may result from a workload that causes the allocator to fragment memory a lot. If the problem is a large peak memory, then there is no issue. Otherwise, make sure you are using the Jemalloc allocator and not the default libc malloc. Note: The currently used allocator is \"%s\".\n\n", ZMALLOC_LIB);
        }
        if (high_alloc_frag) {
            s = sdscatprintf(s," * High allocator fragmentation: This instance has an allocator external fragmentation greater than 1.1. This problem is usually due either to a large peak memory (check if there is a peak memory entry above in the report) or may result from a workload that causes the allocator to fragment memory a lot. You can try enabling 'activedefrag' config option.\n\n");
        }
        if (high_alloc_rss) {
            s = sdscatprintf(s," * High allocator RSS overhead: This instance has an RSS memory overhead is greater than 1.1 (this means that the Resident Set Size of the allocator is much larger than the sum what the allocator actually holds). This problem is usually due to a large peak memory (check if there is a peak memory entry above in the report), you can try the MEMORY PURGE command to reclaim it.\n\n");
        }
        if (high_proc_rss) {
            s = sdscatprintf(s," * High process RSS overhead: This instance has non-allocator RSS memory overhead is greater than 1.1 (this means that the Resident Set Size of the Redis process is much larger than the RSS the allocator holds). This problem may be due to Lua scripts or Modules.\n\n");
        }
        if (big_slave_buf) {
            s = sdscat(s," * Big replica buffers: The replica output buffers in this instance are greater than 10MB for each replica (on average). This likely means that there is some replica instance that is struggling receiving data, either because it is too slow or because of networking issues. As a result, data piles on the master output buffers. Please try to identify what replica is not receiving data correctly and why. You can use the INFO output in order to check the replicas delays and the CLIENT LIST command to check the output buffers of each replica.\n\n");
        }
        if (big_client_buf) {
            s = sdscat(s," * Big client buffers: The clients output buffers in this instance are greater than 200K per client (on average). This may result from different causes, like Pub/Sub clients subscribed to channels bot not receiving data fast enough, so that data piles on the Redis instance output buffer, or clients sending commands with large replies or very large sequences of commands in the same pipeline. Please use the CLIENT LIST command in order to investigate the issue if it causes problems in your instance, or to understand better why certain clients are using a big amount of memory.\n\n");
        }
        if (many_scripts) {
            s = sdscat(s," * Many scripts: There seem to be many cached scripts in this instance (more than 1000). This may be because scripts are generated and `EVAL`ed, instead of being parameterized (with KEYS and ARGV), `SCRIPT LOAD`ed and `EVALSHA`ed. Unless `SCRIPT FLUSH` is called periodically, the scripts' caches may end up consuming most of your memory.\n\n");
        }
        s = sdscat(s,"I'm here to keep you safe, Sam. I want to help you.\n");
    }
    freeMemoryOverheadData(mh);
    return s;
}

/* Set the object LRU/LFU depending on server.maxmemory_policy.
 * The lfu_freq arg is only relevant if policy is MAXMEMORY_FLAG_LFU.
 * The lru_idle and lru_clock args are only relevant if policy
 * is MAXMEMORY_FLAG_LRU.
 * Either or both of them may be <0, in that case, nothing is set. */
int objectSetLRUOrLFU(robj *val, long long lfu_freq, long long lru_idle,
                       long long lru_clock, int lru_multiplier) {
    if (server.maxmemory_policy & MAXMEMORY_FLAG_LFU) {
        if (lfu_freq >= 0) {
            serverAssert(lfu_freq <= 255);
            val->lru = (LFUGetTimeInMinutes()<<8) | lfu_freq;
            return 1;
        }
    } else if (lru_idle >= 0) {
        /* Provided LRU idle time is in seconds. Scale
         * according to the LRU clock resolution this Redis
         * instance was compiled with (normally 1000 ms, so the
         * below statement will expand to lru_idle*1000/1000. */
        lru_idle = lru_idle*lru_multiplier/LRU_CLOCK_RESOLUTION;
        long lru_abs = lru_clock - lru_idle; /* Absolute access time. */
        /* If the LRU field underflows (since LRU it is a wrapping
         * clock), the best we can do is to provide a large enough LRU
         * that is half-way in the circlular LRU clock we use: this way
         * the computed idle time for this object will stay high for quite
         * some time. */
        if (lru_abs < 0)
            lru_abs = (lru_clock+(LRU_CLOCK_MAX/2)) % LRU_CLOCK_MAX;
        val->lru = lru_abs;
        return 1;
    }
    return 0;
}

/* ======================= The OBJECT and MEMORY commands =================== */

/* This is a helper function for the OBJECT command. We need to lookup keys
 * without any modification of LRU or other parameters. */
robj *objectCommandLookup(client *c, robj *key) {
    dictEntry *de;

    if ((de = dictFind(c->db->dict,key->ptr)) == NULL) return NULL;
    return (robj*) dictGetVal(de);
}

robj *objectCommandLookupOrReply(client *c, robj *key, robj *reply) {
    robj *o = objectCommandLookup(c,key);

    if (!o) addReply(c, reply);
    return o;
}

/* Object command allows to inspect the internals of an Redis Object.
 * Usage: OBJECT <refcount|encoding|idletime|freq> <key> */
void objectCommand(client *c) {
    robj *o;

    if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr,"help")) {
        const char *help[] = {
"ENCODING <key> -- Return the kind of internal representation used in order to store the value associated with a key.",
"FREQ <key> -- Return the access frequency index of the key. The returned integer is proportional to the logarithm of the recent access frequency of the key.",
"IDLETIME <key> -- Return the idle time of the key, that is the approximated number of seconds elapsed since the last access to the key.",
"REFCOUNT <key> -- Return the number of references of the value associated with the specified key.",
NULL
        };
        addReplyHelp(c, help);
    } else if (!strcasecmp(c->argv[1]->ptr,"refcount") && c->argc == 3) {
        if ((o = objectCommandLookupOrReply(c,c->argv[2],shared.null[c->resp]))
                == NULL) return;
        addReplyLongLong(c,o->refcount);
    } else if (!strcasecmp(c->argv[1]->ptr,"encoding") && c->argc == 3) {
        if ((o = objectCommandLookupOrReply(c,c->argv[2],shared.null[c->resp]))
                == NULL) return;
        addReplyBulkCString(c,strEncoding(o->encoding));
    } else if (!strcasecmp(c->argv[1]->ptr,"idletime") && c->argc == 3) {
        if ((o = objectCommandLookupOrReply(c,c->argv[2],shared.null[c->resp]))
                == NULL) return;
        if (server.maxmemory_policy & MAXMEMORY_FLAG_LFU) {
            addReplyError(c,"An LFU maxmemory policy is selected, idle time not tracked. Please note that when switching between policies at runtime LRU and LFU data will take some time to adjust.");
            return;
        }
        addReplyLongLong(c,estimateObjectIdleTime(o)/1000);
    } else if (!strcasecmp(c->argv[1]->ptr,"freq") && c->argc == 3) {
        if ((o = objectCommandLookupOrReply(c,c->argv[2],shared.null[c->resp]))
                == NULL) return;
        if (!(server.maxmemory_policy & MAXMEMORY_FLAG_LFU)) {
            addReplyError(c,"An LFU maxmemory policy is not selected, access frequency not tracked. Please note that when switching between policies at runtime LRU and LFU data will take some time to adjust.");
            return;
        }
        /* LFUDecrAndReturn should be called
         * in case of the key has not been accessed for a long time,
         * because we update the access time only
         * when the key is read or overwritten. */
        addReplyLongLong(c,LFUDecrAndReturn(o));
    } else {
        addReplySubcommandSyntaxError(c);
    }
}

/* The memory command will eventually be a complete interface for the
 * memory introspection capabilities of Redis.
 *
 * Usage: MEMORY usage <key> */
void memoryCommand(client *c) {
    if (!strcasecmp(c->argv[1]->ptr,"help") && c->argc == 2) {
        const char *help[] = {
"DOCTOR - Return memory problems reports.",
"MALLOC-STATS -- Return internal statistics report from the memory allocator.",
"PURGE -- Attempt to purge dirty pages for reclamation by the allocator.",
"STATS -- Return information about the memory usage of the server.",
"USAGE <key> [SAMPLES <count>] -- Return memory in bytes used by <key> and its value. Nested values are sampled up to <count> times (default: 5).",
NULL
        };
        addReplyHelp(c, help);
    } else if (!strcasecmp(c->argv[1]->ptr,"usage") && c->argc >= 3) {
        dictEntry *de;
        long long samples = OBJ_COMPUTE_SIZE_DEF_SAMPLES;
        for (int j = 3; j < c->argc; j++) {
            if (!strcasecmp(c->argv[j]->ptr,"samples") &&
                j+1 < c->argc)
            {
                if (getLongLongFromObjectOrReply(c,c->argv[j+1],&samples,NULL)
                     == C_ERR) return;
                if (samples < 0) {
                    addReply(c,shared.syntaxerr);
                    return;
                }
                if (samples == 0) samples = LLONG_MAX;;
                j++; /* skip option argument. */
            } else {
                addReply(c,shared.syntaxerr);
                return;
            }
        }
        if ((de = dictFind(c->db->dict,c->argv[2]->ptr)) == NULL) {
            addReplyNull(c);
            return;
        }
        size_t usage = objectComputeSize(dictGetVal(de),samples);
        usage += sdsAllocSize(dictGetKey(de));
        usage += sizeof(dictEntry);
        addReplyLongLong(c,usage);
    } else if (!strcasecmp(c->argv[1]->ptr,"stats") && c->argc == 2) {
        struct redisMemOverhead *mh = getMemoryOverheadData();

        addReplyMapLen(c,25+mh->num_dbs);

        addReplyBulkCString(c,"peak.allocated");
        addReplyLongLong(c,mh->peak_allocated);

        addReplyBulkCString(c,"total.allocated");
        addReplyLongLong(c,mh->total_allocated);

        addReplyBulkCString(c,"startup.allocated");
        addReplyLongLong(c,mh->startup_allocated);

        addReplyBulkCString(c,"replication.backlog");
        addReplyLongLong(c,mh->repl_backlog);

        addReplyBulkCString(c,"clients.slaves");
        addReplyLongLong(c,mh->clients_slaves);

        addReplyBulkCString(c,"clients.normal");
        addReplyLongLong(c,mh->clients_normal);

        addReplyBulkCString(c,"aof.buffer");
        addReplyLongLong(c,mh->aof_buffer);

        addReplyBulkCString(c,"lua.caches");
        addReplyLongLong(c,mh->lua_caches);

        for (size_t j = 0; j < mh->num_dbs; j++) {
            char dbname[32];
            snprintf(dbname,sizeof(dbname),"db.%zd",mh->db[j].dbid);
            addReplyBulkCString(c,dbname);
            addReplyMapLen(c,2);

            addReplyBulkCString(c,"overhead.hashtable.main");
            addReplyLongLong(c,mh->db[j].overhead_ht_main);

            addReplyBulkCString(c,"overhead.hashtable.expires");
            addReplyLongLong(c,mh->db[j].overhead_ht_expires);
        }

        addReplyBulkCString(c,"overhead.total");
        addReplyLongLong(c,mh->overhead_total);

        addReplyBulkCString(c,"keys.count");
        addReplyLongLong(c,mh->total_keys);

        addReplyBulkCString(c,"keys.bytes-per-key");
        addReplyLongLong(c,mh->bytes_per_key);

        addReplyBulkCString(c,"dataset.bytes");
        addReplyLongLong(c,mh->dataset);

        addReplyBulkCString(c,"dataset.percentage");
        addReplyDouble(c,mh->dataset_perc);

        addReplyBulkCString(c,"peak.percentage");
        addReplyDouble(c,mh->peak_perc);

        addReplyBulkCString(c,"allocator.allocated");
        addReplyLongLong(c,server.cron_malloc_stats.allocator_allocated);

        addReplyBulkCString(c,"allocator.active");
        addReplyLongLong(c,server.cron_malloc_stats.allocator_active);

        addReplyBulkCString(c,"allocator.resident");
        addReplyLongLong(c,server.cron_malloc_stats.allocator_resident);

        addReplyBulkCString(c,"allocator-fragmentation.ratio");
        addReplyDouble(c,mh->allocator_frag);

        addReplyBulkCString(c,"allocator-fragmentation.bytes");
        addReplyLongLong(c,mh->allocator_frag_bytes);

        addReplyBulkCString(c,"allocator-rss.ratio");
        addReplyDouble(c,mh->allocator_rss);

        addReplyBulkCString(c,"allocator-rss.bytes");
        addReplyLongLong(c,mh->allocator_rss_bytes);

        addReplyBulkCString(c,"rss-overhead.ratio");
        addReplyDouble(c,mh->rss_extra);

        addReplyBulkCString(c,"rss-overhead.bytes");
        addReplyLongLong(c,mh->rss_extra_bytes);

        addReplyBulkCString(c,"fragmentation"); /* this is the total RSS overhead, including fragmentation */
        addReplyDouble(c,mh->total_frag); /* it is kept here for backwards compatibility */

        addReplyBulkCString(c,"fragmentation.bytes");
        addReplyLongLong(c,mh->total_frag_bytes);

        freeMemoryOverheadData(mh);
    } else if (!strcasecmp(c->argv[1]->ptr,"malloc-stats") && c->argc == 2) {
#if defined(USE_JEMALLOC)
        sds info = sdsempty();
        je_malloc_stats_print(inputCatSds, &info, NULL);
        addReplyVerbatim(c,info,sdslen(info),"txt");
        sdsfree(info);
#else
        addReplyBulkCString(c,"Stats not supported for the current allocator");
#endif
    } else if (!strcasecmp(c->argv[1]->ptr,"doctor") && c->argc == 2) {
        sds report = getMemoryDoctorReport();
        addReplyVerbatim(c,report,sdslen(report),"txt");
        sdsfree(report);
    } else if (!strcasecmp(c->argv[1]->ptr,"purge") && c->argc == 2) {
        if (jemalloc_purge() == 0)
            addReply(c, shared.ok);
        else
            addReplyError(c, "Error purging dirty pages");
    } else {
        addReplyErrorFormat(c, "Unknown subcommand or wrong number of arguments for '%s'. Try MEMORY HELP", (char*)c->argv[1]->ptr);
    }
}
