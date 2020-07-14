/*
 *该文件中的代码主要是redis中string数据类型的命令实现
 */
#include "server.h"
#include <math.h> /* isnan(), isinf() */

/*-----------------------------------------------------------------------------
 * String Commands（字符串命令）
 *----------------------------------------------------------------------------*/

///检测字符串的长度，如果字符串的长度大于512M，超过了限制，直接返回0，否则返回1
static int checkStringLength(client *c, long long size) {
    if (size > 512*1024*1024) {
        addReplyError(c,"string exceeds maximum allowed size (512MB)");
        return C_ERR;
    }
    return C_OK;
}

/* The setGenericCommand() function implements the SET operation with different
 * options and variants. This function is called in order to implement the
 * following commands: SET, SETEX, PSETEX, SETNX.
 *
 * 'flags' changes the behavior of the command (NX or XX, see below).
 *
 * 'expire' represents an expire to set in form of a Redis object as passed
 * by the user. It is interpreted according to the specified 'unit'.
 *
 * 'ok_reply' and 'abort_reply' is what the function will reply to the client
 * if the operation is performed, or when it is not because of NX or
 * XX flags.
 *
 * If ok_reply is NULL "+OK" is used.
 * If abort_reply is NULL, "$-1" is used. */

#define OBJ_SET_NO_FLAGS 0
#define OBJ_SET_NX (1<<0)         ///1. 在key不存在的情况下设置
#define OBJ_SET_XX (1<<1)         ///2. 在key存在的情况下设置
#define OBJ_SET_EX (1<<2)         ///4. 以秒为单位设置键的过期时间
#define OBJ_SET_PX (1<<3)         ///8. 以毫秒为单位设置键的过期时间
#define OBJ_SET_KEEPTTL (1<<4)    ///16. 设置并保留ttl 

///核心方法
///该函数实现了：set， setnx， psetex， setex命令
///flags 标志位可以是NX或者XX，
///expre 表示用户设置键的过期时间，它的unit指定
///ok_reply和abort_reply保存者对客户端的回应消息，NX、XX也会改变
///如果ok_reply为NULL，则使用“+OK”。
///如果abort_reply为NULL，则使用“$-1”。
void setGenericCommand(client *c, int flags, robj *key, robj *val, robj *expire, int unit, robj *ok_reply, robj *abort_reply) {
    long long milliseconds = 0; /* initialized to avoid any harmness warning */

    if (expire) { ///如果设置了键的过期时间
        if (getLongLongFromObjectOrReply(c, expire, &milliseconds, NULL) != C_OK) ///从expr对象中获取存活时间，如果获取失败，则直接返回
            return;
        if (milliseconds <= 0) { ///如果获取的存活时间小于0，返回异常信息给客户端
            addReplyErrorFormat(c,"invalid expire time in %s",c->cmd->name);
            return;
        }
        if (unit == UNIT_SECONDS) milliseconds *= 1000; ///如果存活时间单位为秒，将其转化为毫秒
    }
	
	///lookupKeyWrite函数是为执行写操作而取出key的值对象，如果设置了NX(表示该键要不存在)，在库中区查找该key。
	///如果设置了XX（表示该key要存在），也要去数据库中查询该key值。如果是NX但是数据库中存在这个值，或者是XX但是
    ///数据库中不存在该值，则直接返回abort_reply给客户端
    if ((flags & OBJ_SET_NX && lookupKeyWrite(c->db,key) != NULL) ||
        (flags & OBJ_SET_XX && lookupKeyWrite(c->db,key) == NULL))
    {
        addReply(c, abort_reply ? abort_reply : shared.null[c->resp]); ///增加返回信息
        return;
    }
    genericSetKey(c,c->db,key,val,flags & OBJ_SET_KEEPTTL,1); ///为key设置对应的value值
    server.dirty++; ///设置服务器的dirty计数，每修改一次key，服务器的dirty计数加1
    if (expire) setExpire(c,c->db,key,mstime()+milliseconds); ///如果设置了过期时间，就给这个key设置过期时间
    notifyKeyspaceEvent(NOTIFY_STRING,"set",key,c->db->id); ///发送set通知，给订阅了服务器的客户端发送通知(订阅模式才会用到)
    if (expire) notifyKeyspaceEvent(NOTIFY_GENERIC, 
        "expire",key,c->db->id); ///如果对key设置了过期时间，会发送expire通知给订阅服务器的客户端发送通知
    addReply(c, ok_reply ? ok_reply : shared.ok); ///给客户端回复消息，操作成功
}

/// SET key value [NX] [XX] [KEEPTTL] [EX <seconds>] [PX <milliseconds>] 
/// set命令相关内容
void setCommand(client *c) {
    int j;
    robj *expire = NULL;
    int unit = UNIT_SECONDS; ///初始化时间单位，为秒
    int flags = OBJ_SET_NO_FLAGS; ///初始化flags，为OBJ_SET_NO_FLAGS

    for (j = 3; j < c->argc; j++) { ///获取传入的set参数
        char *a = c->argv[j]->ptr;  ///从第四个字符开始保存地址，因为前三个为 "set" 
        robj *next = (j == c->argc-1) ? NULL : c->argv[j+1]; 

        if ((a[0] == 'n' || a[0] == 'N') &&
            (a[1] == 'x' || a[1] == 'X') && a[2] == '\0' &&
            !(flags & OBJ_SET_XX)) ///如果传入的参数为set NX（nx）类型 并且 flags不是XX类型 
        { 
            flags |= OBJ_SET_NX; ///设置flags的标志位为NX
        } else if ((a[0] == 'x' || a[0] == 'X') &&
                   (a[1] == 'x' || a[1] == 'X') && a[2] == '\0' &&
                   !(flags & OBJ_SET_NX))///如果传入的参数为XX（xx）类型，并且flgas不是NX类型
        {
            flags |= OBJ_SET_XX; ///设置flags的标志位为XX
        } else if (!strcasecmp(c->argv[j]->ptr,"KEEPTTL") &&
                   !(flags & OBJ_SET_EX) && !(flags & OBJ_SET_PX)) ///如果参数是KEEPTTl，并且falgs不是EX、PX类型
        {
            flags |= OBJ_SET_KEEPTTL; ///设置flags标志位为KEEPTTl
        } else if ((a[0] == 'e' || a[0] == 'E') &&
                   (a[1] == 'x' || a[1] == 'X') && a[2] == '\0' &&
                   !(flags & OBJ_SET_KEEPTTL) &&
                   !(flags & OBJ_SET_PX) && next) ///如果参数为EX(ex)类型并且flags不是KEEPTTL
        {
            flags |= OBJ_SET_EX; ///设置falgs标志位为EX
            unit = UNIT_SECONDS; ///设置过期时间单位为秒
            expire = next;  ///设置过期时间
            j++;  ///因为获取next，所以直接跳过next下标
        } else if ((a[0] == 'p' || a[0] == 'P') &&
                   (a[1] == 'x' || a[1] == 'X') && a[2] == '\0' &&
                   !(flags & OBJ_SET_KEEPTTL) &&
                   !(flags & OBJ_SET_EX) && next) ///如果参数是PX(px),并且falgs标志位不是KEEPTTL
        {
            flags |= OBJ_SET_PX; ///设置falgs的标志为PX
            unit = UNIT_MILLISECONDS; ///设置过期时间为毫秒
            expire = next; ///保存过期时间
            j++; ///跳过next下标
        } else {
            addReply(c,shared.syntaxerr); ///不满足上面的条件，直接返回错误信息给客户端，语法错误
            return;
        }
    }

    c->argv[2] = tryObjectEncoding(c->argv[2]); ///对value进行编码
    setGenericCommand(c,flags,c->argv[1],c->argv[2],expire,unit,NULL,NULL); ///将数据保存到数据库中
}

///setnx 命令的实现
void setnxCommand(client *c) {
    c->argv[2] = tryObjectEncoding(c->argv[2]);
    setGenericCommand(c,OBJ_SET_NX,c->argv[1],c->argv[2],NULL,0,shared.cone,shared.czero);
}

///setex 命令的实现
void setexCommand(client *c) {
    c->argv[3] = tryObjectEncoding(c->argv[3]);
    setGenericCommand(c,OBJ_SET_NO_FLAGS,c->argv[1],c->argv[3],c->argv[2],UNIT_SECONDS,NULL,NULL);
}

///psetex 命令的实现
void psetexCommand(client *c) {
    c->argv[3] = tryObjectEncoding(c->argv[3]);
    setGenericCommand(c,OBJ_SET_NO_FLAGS,c->argv[1],c->argv[3],c->argv[2],UNIT_MILLISECONDS,NULL,NULL);
}

///get 命令的底层实现
int getGenericCommand(client *c) {
    robj *o;
	
	///通过lookupKeyReadOrReply()函数从数据库中获取值，如果获取到的值为空，直接返回C_OK
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.null[c->resp])) == NULL)
        return C_OK;

    if (o->type != OBJ_STRING) { ///如果o的type不是字符串类型，则返回类型错误信息
        addReply(c,shared.wrongtypeerr);
        return C_ERR;
    } else { ///如果查询到了对象，并且这个对象不为NULL，把这个对象返回给客户端，并返回C_OK
        addReplyBulk(c,o);
        return C_OK;
    }
}

///get 命令的实现
void getCommand(client *c) {
    getGenericCommand(c);
}

///getset命令的实现
void getsetCommand(client *c) {
    
	if (getGenericCommand(c) == C_ERR) return;///用get命令获取值，并将获取到的值传给客户端
    c->argv[2] = tryObjectEncoding(c->argv[2]); ///对新的值尝试进行编码优化
    setKey(c,c->db,c->argv[1],c->argv[2]); ///进行值替换操作
    notifyKeyspaceEvent(NOTIFY_STRING,"set",c->argv[1],c->db->id); ///Set事件通知，通知给订阅服务器的客户端
    server.dirty++; ///服务器的dirty计数器+1
}

///setrange命令的实现
void setrangeCommand(client *c) {
    
	robj *o;
    long offset;
    sds value = c->argv[3]->ptr; ///获取要设置的value
	///从c对象中取出long 类型的值，并将它保存到offset中，如果发生错误，直接返回
    if (getLongFromObjectOrReply(c,c->argv[2],&offset,NULL) != C_OK)
        return;
	
	///如果offset小于0，该数值不符合要求，直接返回
    if (offset < 0) {
        addReplyError(c,"offset is out of range");
        return;
    }
	
	///从数据库中找出对应key的值
    o = lookupKeyWrite(c->db,c->argv[1]);
    if (o == NULL) { ///如果这个key在数据库中不存在
        /* Return 0 when setting nothing on a non-existing string */
        if (sdslen(value) == 0) { ///如果用来替换的字符串长度为0 ， 直接返回
            addReply(c,shared.czero); 
            return;
        }

        /* Return when the resulting string exceeds allowed size */
		///检测字符串的长度是否符合要求，如果不符合，直接返回
        if (checkStringLength(c,offset+sdslen(value)) != C_OK)
            return;
		
		///创建一个新的对象，并将这个新的对象保存到数据库中
        o = createObject(OBJ_STRING,sdsnewlen(NULL, offset+sdslen(value)));
        dbAdd(c->db,c->argv[1],o);
    } else { ///如果该key在数据库中存在，并且对应的value不为NULL
        size_t olen;

        ///判断key对应的value的类型是不是字符串类型，如果不是，直接返回
        if (checkType(c,o,OBJ_STRING))
            return;

       ///如果用来替换的字符串为空，就什么也不做，直接返回
        olen = stringObjectLen(o);
        if (sdslen(value) == 0) {
            addReplyLongLong(c,olen);
            return;
        }

        ///检测新的字符串大小是否符合要求，如果大于512M，直接返回
        if (checkStringLength(c,offset+sdslen(value)) != C_OK)
            return;

        /* Create a copy when the object is shared or encoded. */
       ///因为要根据value修改key的值，因此如果key原来的值是共享的，需要解除共享，新创建一个值对象与key对应
        o = dbUnshareStringValue(c->db,c->argv[1],o);
    }

    if (sdslen(value) > 0) { ///如果用来替换的字符串不为空
        o->ptr = sdsgrowzero(o->ptr,offset+sdslen(value)); ///将对象o的ptr指针内存大小进行扩容
        memcpy((char*)o->ptr+offset,value,sdslen(value)); ///将替换的字符串拷贝到对象o的ptr指针对应的位置
        signalModifiedKey(c,c->db,c->argv[1]); ///当数据库的键被改动，则会调用该函数发送信号
        notifyKeyspaceEvent(NOTIFY_STRING,
            "setrange",c->argv[1],c->db->id); ///发送setrange类型的通知给订阅的客户端
        server.dirty++; ///服务器的dirty计数器+1
    }
    addReplyLongLong(c,sdslen(o->ptr)); ///将新的值发送给客户端
}

///getrange 命令的实现
void getrangeCommand(client *c) {
    
	robj *o;
    long long start, end;
    char *str, llbuf[32];
    size_t strlen;
	
	///获取start的值
    if (getLongLongFromObjectOrReply(c,c->argv[2],&start,NULL) != C_OK)
        return;
    ///获取end的值
    if (getLongLongFromObjectOrReply(c,c->argv[3],&end,NULL) != C_OK)
        return;
	
	///从数据库中读出key对应的value，并保存在对象o中。
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.emptybulk)) == NULL ||
        checkType(c,o,OBJ_STRING)) return; ///检测o的类型是不是字符串类型，如果不是就直接返回

    if (o->encoding == OBJ_ENCODING_INT) { ///如果o的编码格式为int类型的编码
        str = llbuf;
        strlen = ll2string(llbuf,sizeof(llbuf),(long)o->ptr); ///将它转化为字符串，
    } else { ///如果是原始编码或者字符串编码
        str = o->ptr; ///获取字符串
        strlen = sdslen(str); ///计算出字符串的长度
    }

    /* Convert negative indexes */
    if (start < 0 && end < 0 && start > end) { ///如果设置的开始位置和结束位置不满足条件，直接返回
        addReply(c,shared.emptybulk);
        return;
    }
    if (start < 0) start = strlen+start; ///如果start<0,表示倒数第start个位置，将他转化为正数
    if (end < 0) end = strlen+end; ///如果end <0,表示倒数第end个位置，将它转化为正数

	///如果经过上面的转化，start和end的值还是小于0，就直接让其等于0
    if (start < 0) start = 0; 
    if (end < 0) end = 0;
    if ((unsigned long long)end >= strlen) end = strlen-1; ///如果end查过了字符串的范围，就让它等于字符串长度-1

    /* Precondition: end >= 0 && end < strlen, so the only condition where
     * nothing can be returned is: start > end. */
    if (start > end || strlen == 0) {  ///如果字符串为空或者start > end,直接返回空字符串给客户端
        addReply(c,shared.emptybulk);
    } else {
        addReplyBulkCBuffer(c,(char*)str+start,end-start+1); ///苟泽，将截取的字符串返回给客户端
    }
}

///mget命令的实现，获取多个key对应的值
void mgetCommand(client *c) {
    int j;

    addReplyArrayLen(c,c->argc-1); ///发送key的个数给客户端
    for (j = 1; j < c->argc; j++) { ///从下标为1的key进行遍历
        robj *o = lookupKeyRead(c->db,c->argv[j]); ///查询每一个key对应的对象
        if (o == NULL) { ///如果对象为空，则直接返回空信息给客户端
            addReplyNull(c);
        } else {
            if (o->type != OBJ_STRING) { ///对象的类型不是String类型
                addReplyNull(c); ///返回空信息给客户攒
            } else {
                addReplyBulk(c,o); ///如果对象是字符串类型，将key对应的value发送给客户端
            }
        }
    }
}

///mset命令的实现
void msetGenericCommand(client *c, int nx) {
    int j;

    if ((c->argc % 2) == 0) { ///如果发送的参数是偶数个，直接返回
        addReplyError(c,"wrong number of arguments for MSET");
        return;
    }

    /* Handle the NX flag. The MSETNX semantic is to return zero and don't
     * set anything if at least one key alerady exists. */
    if (nx) { ///如果需要设置NX
        for (j = 1; j < c->argc; j += 2) { ///从参数中下标为1的位置开始遍历
            if (lookupKeyWrite(c->db,c->argv[j]) != NULL) { ///从数据库中查询key是否存在
                addReply(c, shared.czero); ///如果存在就直接返回
                return;
            }
        }
    }

    for (j = 1; j < c->argc; j += 2) { ///遍历参数中所有的值
        c->argv[j+1] = tryObjectEncoding(c->argv[j+1]); ///将参数中传入的value进行编码优化
        setKey(c,c->db,c->argv[j],c->argv[j+1]); ///对key-value插入到数据库中
        notifyKeyspaceEvent(NOTIFY_STRING,"set",c->argv[j],c->db->id); ///发送set类型的通知给订阅了服务器的客户端
    } 
    server.dirty += (c->argc-1)/2;///修改服务器的dirty的计数
    addReply(c, nx ? shared.cone : shared.ok); ///给客户端发送操作成功的信息
}

///mset命令的实现
void msetCommand(client *c) {
    msetGenericCommand(c,0);
}

///msetnx命令的是吸纳
void msetnxCommand(client *c) {
    msetGenericCommand(c,1);
}

///incr、decr命令的实现
void incrDecrCommand(client *c, long long incr) {
   
	long long value, oldvalue;
    robj *o, *new;

    o = lookupKeyWrite(c->db,c->argv[1]); ///从数据库中找出对应key的value对象
    if (o != NULL && checkType(c,o,OBJ_STRING)) return; ///如果对象为NULL或者该对象不是字符串类型，直接返回
    if (getLongLongFromObjectOrReply(c,o,&value,NULL) != C_OK) return; ///将字符串对象转化为long long类型，并保存在value中，如果失败，直接返回

    oldvalue = value; ///将value的值保存在oldvalue中
    if ((incr < 0 && oldvalue < 0 && incr < (LLONG_MIN-oldvalue)) || 
        (incr > 0 && oldvalue > 0 && incr > (LLONG_MAX-oldvalue))) { ///如果value的变化参数不符合要求，就直接返回
        addReplyError(c,"increment or decrement would overflow");
        return;
    }
    value += incr; ///修改value的值

	///如果对象o不为空并且引用计数为1 并且他的编码格式为整数编码 并且
	///value < 0或者value大于OBJ_SHARED_INTEGERS 并且
	///value 在Long类型的数据范围内
    if (o && o->refcount == 1 && o->encoding == OBJ_ENCODING_INT &&
        (value < 0 || value >= OBJ_SHARED_INTEGERS) &&
        value >= LONG_MIN && value <= LONG_MAX)
    {
        new = o; ///讲o赋值给new
        o->ptr = (void*)((long)value); ///修改o的值
    } else { ///如果不满上面的条件
        new = createStringObjectFromLongLongForValue(value); ///用value的值创建一个字符串
        if (o) { ///如果o不为空
            dbOverwrite(c->db,c->argv[1],new); ///讲new写回到数据库中，并覆盖o
        } else { ///如果o为空，就插入一个新的值new
            dbAdd(c->db,c->argv[1],new);
        }
    }
    signalModifiedKey(c,c->db,c->argv[1]); ///发送数据库key改动的信息
    notifyKeyspaceEvent(NOTIFY_STRING,"incrby",c->argv[1],c->db->id);///发送incrby类型的通知给订阅了服务器的客户端
    server.dirty++; ///服务器的dirty计数+1
    addReply(c,shared.colon); ///f发送信息给客户端
    addReply(c,new);
    addReply(c,shared.crlf);
}

///incr命令的实现
void incrCommand(client *c) {
    incrDecrCommand(c,1);
}

///decr命令的是吸纳
void decrCommand(client *c) {
    incrDecrCommand(c,-1);
}

///incrby命令的实现
void incrbyCommand(client *c) {
    long long incr;

    if (getLongLongFromObjectOrReply(c, c->argv[2], &incr, NULL) != C_OK) return;
    incrDecrCommand(c,incr);
}

///decrby命令的实现
void decrbyCommand(client *c) {
    long long incr;

    if (getLongLongFromObjectOrReply(c, c->argv[2], &incr, NULL) != C_OK) return;
    incrDecrCommand(c,-incr);
}

///incrbyfloat 命令的实现
void incrbyfloatCommand(client *c) {
    long double incr, value;
    robj *o, *new, *aux1, *aux2;

    o = lookupKeyWrite(c->db,c->argv[1]); ///从数据库中找出对应key的vaule对象
    if (o != NULL && checkType(c,o,OBJ_STRING)) return; ///如果key不为空并且key不是字符串类型，直接返回
    if (getLongDoubleFromObjectOrReply(c,o,&value,NULL) != C_OK || ///讲字符串转化为long double类型，保存在value中
        getLongDoubleFromObjectOrReply(c,c->argv[2],&incr,NULL) != C_OK) ///将变化的incr参数读取出来，保存到incr中
        return; ///如果上面的两个步骤有一个失败了，就直接返回

    value += incr; ///对vlaue的数值进行操作
    if (isnan(value) || isinf(value)) { ///如果变化后的value不符合要求，就直接返回错误信息
        addReplyError(c,"increment would produce NaN or Infinity");
        return;
    }
    new = createStringObjectFromLongDouble(value,1); ///讲value转化成为一个新的字符串对象
    if (o) ///如果o不为空
        dbOverwrite(c->db,c->argv[1],new); ///用new的值讲o进行覆盖
    else
        dbAdd(c->db,c->argv[1],new); ///如果o为空，直接插入一个新的对象
    signalModifiedKey(c,c->db,c->argv[1]); ///发送数据库key变化的通知
    notifyKeyspaceEvent(NOTIFY_STRING,"incrbyfloat",c->argv[1],c->db->id); ///发送incrbyfloat类型的通知给订阅了服务器的客户端
    server.dirty++; ///数据库的dirty计数+1
    addReplyBulk(c,new); ///返回信息给客户端

   ///始终将INCRBYFLOAT复制为带有最终值的SET命令，以确保浮点精度或格式上的差异不会在副本中或AOF重新启动后产生差异。
    aux1 = createStringObject("SET",3); ///设置set字符串
    rewriteClientCommandArgument(c,0,aux1); ///将Client中的incrFloat命令用Set命令进行替换
    decrRefCount(aux1); ///将aux1对象的引用计数-1.由于本来就是1，这里就相当于释放aux1这个对象
    rewriteClientCommandArgument(c,2,new); ///讲incrFloat中操作的参数值替换为新的value值
    aux2 = createStringObject("KEEPTTL",7); ///创建一个新的字符串对象，该对象为KEETTTL
    rewriteClientCommandArgument(c,3,aux2); ///讲c中的第三个参数有aux2替换
    decrRefCount(aux2);
}

///appned命令的实现
void appendCommand(client *c) {
    
	size_t totlen;
    robj *o, *append;

    o = lookupKeyWrite(c->db,c->argv[1]); ///从数据库中找出对应key的value对象
    if (o == NULL) { ///如果对象为空
        /* Create the key */
        c->argv[2] = tryObjectEncoding(c->argv[2]); ///对将要append的对象进行编码优化
        dbAdd(c->db,c->argv[1],c->argv[2]); ///讲要追加的append对象直接保存数据库中
        incrRefCount(c->argv[2]); ///修改该对象的引用计数
        totlen = stringObjectLen(c->argv[2]); ///获取字符串的长度
    } else { ///如果对象不为空，表示该key值已经存在
        /* Key exists, check type */
        if (checkType(c,o,OBJ_STRING)) ///检测o类型是不是字符串类型，如果不是，则直接返回
            return;

        /* "append" is an argument, so always an sds */
        append = c->argv[2]; ///获取要追加的字符串
        totlen = stringObjectLen(o)+sdslen(append->ptr); ///获取如果追加操作完毕后字符串的长度
        if (checkStringLength(c,totlen) != C_OK) ///如果字符串的长度大于521M，则直接返回
            return;

        o = dbUnshareStringValue(c->db,c->argv[1],o); ///将对象o进行追加操作
        o->ptr = sdscatlen(o->ptr,append->ptr,sdslen(append->ptr)); ///设置对象的新的值
        totlen = sdslen(o->ptr); ///获取字符串对象的长度
    }
    signalModifiedKey(c,c->db,c->argv[1]); ///发送数据库key有修改的信号
    notifyKeyspaceEvent(NOTIFY_STRING,"append",c->argv[1],c->db->id); ///发送append类型的通知给订阅了服务器的客户端
    server.dirty++; ///服务器的dirty计数器+1
    addReplyLongLong(c,totlen); ///返回信息给客户端
}

///strlen命令的实现
void strlenCommand(client *c) {
    robj *o;
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL || ///在数据库中查询对应key的vlaue
        checkType(c,o,OBJ_STRING)) return; //检测该value的类型，如果不是字符串类型，就直接返回
    addReplyLongLong(c,stringObjectLen(o)); ///获取字符串的长度，并将其返回个客户端
}
/*****************************************************************************************/
 *   redis字符串命令实现到这里就正式结束了
/*****************************************************************************************/


///下面是关于字符串的复杂算法，下一阶段再来讲述
/* STRALGO -- Implement complex algorithms on strings.
 *
 * STRALGO <algorithm> ... arguments ... */
void stralgoLCS(client *c);     /* This implements the LCS algorithm. */
void stralgoCommand(client *c) {
    /* Select the algorithm. */
    if (!strcasecmp(c->argv[1]->ptr,"lcs")) {
        stralgoLCS(c);
    } else {
        addReply(c,shared.syntaxerr);
    }
}

/* STRALGO <algo> [IDX] [MINMATCHLEN <len>] [WITHMATCHLEN]
 *     STRINGS <string> <string> | KEYS <keya> <keyb>
 */
void stralgoLCS(client *c) {
    uint32_t i, j;
    long long minmatchlen = 0;
    sds a = NULL, b = NULL;
    int getlen = 0, getidx = 0, withmatchlen = 0;
    robj *obja = NULL, *objb = NULL;

    for (j = 2; j < (uint32_t)c->argc; j++) {
        char *opt = c->argv[j]->ptr;
        int moreargs = (c->argc-1) - j;

        if (!strcasecmp(opt,"IDX")) {
            getidx = 1;
        } else if (!strcasecmp(opt,"LEN")) {
            getlen = 1;
        } else if (!strcasecmp(opt,"WITHMATCHLEN")) {
            withmatchlen = 1;
        } else if (!strcasecmp(opt,"MINMATCHLEN") && moreargs) {
            if (getLongLongFromObjectOrReply(c,c->argv[j+1],&minmatchlen,NULL)
                != C_OK) goto cleanup;
            if (minmatchlen < 0) minmatchlen = 0;
            j++;
        } else if (!strcasecmp(opt,"STRINGS") && moreargs > 1) {
            if (a != NULL) {
                addReplyError(c,"Either use STRINGS or KEYS");
                goto cleanup;
            }
            a = c->argv[j+1]->ptr;
            b = c->argv[j+2]->ptr;
            j += 2;
        } else if (!strcasecmp(opt,"KEYS") && moreargs > 1) {
            if (a != NULL) {
                addReplyError(c,"Either use STRINGS or KEYS");
                goto cleanup;
            }
            obja = lookupKeyRead(c->db,c->argv[j+1]);
            objb = lookupKeyRead(c->db,c->argv[j+2]);
            if ((obja && obja->type != OBJ_STRING) ||
                (objb && objb->type != OBJ_STRING))
            {
                addReplyError(c,
                    "The specified keys must contain string values");
                /* Don't cleanup the objects, we need to do that
                 * only after callign getDecodedObject(). */
                obja = NULL;
                objb = NULL;
                goto cleanup;
            }
            obja = obja ? getDecodedObject(obja) : createStringObject("",0);
            objb = objb ? getDecodedObject(objb) : createStringObject("",0);
            a = obja->ptr;
            b = objb->ptr;
            j += 2;
        } else {
            addReply(c,shared.syntaxerr);
            goto cleanup;
        }
    }

    /* Complain if the user passed ambiguous parameters. */
    if (a == NULL) {
        addReplyError(c,"Please specify two strings: "
                        "STRINGS or KEYS options are mandatory");
        goto cleanup;
    } else if (getlen && getidx) {
        addReplyError(c,
            "If you want both the length and indexes, please "
            "just use IDX.");
        goto cleanup;
    }

    /* Compute the LCS using the vanilla dynamic programming technique of
     * building a table of LCS(x,y) substrings. */
    uint32_t alen = sdslen(a);
    uint32_t blen = sdslen(b);

    /* Setup an uint32_t array to store at LCS[i,j] the length of the
     * LCS A0..i-1, B0..j-1. Note that we have a linear array here, so
     * we index it as LCS[j+(blen+1)*j] */
    uint32_t *lcs = zmalloc((alen+1)*(blen+1)*sizeof(uint32_t));
    #define LCS(A,B) lcs[(B)+((A)*(blen+1))]

    /* Start building the LCS table. */
    for (uint32_t i = 0; i <= alen; i++) {
        for (uint32_t j = 0; j <= blen; j++) {
            if (i == 0 || j == 0) {
                /* If one substring has length of zero, the
                 * LCS length is zero. */
                LCS(i,j) = 0;
            } else if (a[i-1] == b[j-1]) {
                /* The len LCS (and the LCS itself) of two
                 * sequences with the same final character, is the
                 * LCS of the two sequences without the last char
                 * plus that last char. */
                LCS(i,j) = LCS(i-1,j-1)+1;
            } else {
                /* If the last character is different, take the longest
                 * between the LCS of the first string and the second
                 * minus the last char, and the reverse. */
                uint32_t lcs1 = LCS(i-1,j);
                uint32_t lcs2 = LCS(i,j-1);
                LCS(i,j) = lcs1 > lcs2 ? lcs1 : lcs2;
            }
        }
    }

    /* Store the actual LCS string in "result" if needed. We create
     * it backward, but the length is already known, we store it into idx. */
    uint32_t idx = LCS(alen,blen);
    sds result = NULL;        /* Resulting LCS string. */
    void *arraylenptr = NULL; /* Deffered length of the array for IDX. */
    uint32_t arange_start = alen, /* alen signals that values are not set. */
             arange_end = 0,
             brange_start = 0,
             brange_end = 0;

    /* Do we need to compute the actual LCS string? Allocate it in that case. */
    int computelcs = getidx || !getlen;
    if (computelcs) result = sdsnewlen(SDS_NOINIT,idx);

    /* Start with a deferred array if we have to emit the ranges. */
    uint32_t arraylen = 0;  /* Number of ranges emitted in the array. */
    if (getidx) {
        addReplyMapLen(c,2);
        addReplyBulkCString(c,"matches");
        arraylenptr = addReplyDeferredLen(c);
    }

    i = alen, j = blen;
    while (computelcs && i > 0 && j > 0) {
        int emit_range = 0;
        if (a[i-1] == b[j-1]) {
            /* If there is a match, store the character and reduce
             * the indexes to look for a new match. */
            result[idx-1] = a[i-1];

            /* Track the current range. */
            if (arange_start == alen) {
                arange_start = i-1;
                arange_end = i-1;
                brange_start = j-1;
                brange_end = j-1;
            } else {
                /* Let's see if we can extend the range backward since
                 * it is contiguous. */
                if (arange_start == i && brange_start == j) {
                    arange_start--;
                    brange_start--;
                } else {
                    emit_range = 1;
                }
            }
            /* Emit the range if we matched with the first byte of
             * one of the two strings. We'll exit the loop ASAP. */
            if (arange_start == 0 || brange_start == 0) emit_range = 1;
            idx--; i--; j--;
        } else {
            /* Otherwise reduce i and j depending on the largest
             * LCS between, to understand what direction we need to go. */
            uint32_t lcs1 = LCS(i-1,j);
            uint32_t lcs2 = LCS(i,j-1);
            if (lcs1 > lcs2)
                i--;
            else
                j--;
            if (arange_start != alen) emit_range = 1;
        }

        /* Emit the current range if needed. */
        uint32_t match_len = arange_end - arange_start + 1;
        if (emit_range) {
            if (minmatchlen == 0 || match_len >= minmatchlen) {
                if (arraylenptr) {
                    addReplyArrayLen(c,2+withmatchlen);
                    addReplyArrayLen(c,2);
                    addReplyLongLong(c,arange_start);
                    addReplyLongLong(c,arange_end);
                    addReplyArrayLen(c,2);
                    addReplyLongLong(c,brange_start);
                    addReplyLongLong(c,brange_end);
                    if (withmatchlen) addReplyLongLong(c,match_len);
                    arraylen++;
                }
            }
            arange_start = alen; /* Restart at the next match. */
        }
    }

    /* Signal modified key, increment dirty, ... */

    /* Reply depending on the given options. */
    if (arraylenptr) {
        addReplyBulkCString(c,"len");
        addReplyLongLong(c,LCS(alen,blen));
        setDeferredArrayLen(c,arraylenptr,arraylen);
    } else if (getlen) {
        addReplyLongLong(c,LCS(alen,blen));
    } else {
        addReplyBulkSds(c,result);
        result = NULL;
    }

    /* Cleanup. */
    sdsfree(result);
    zfree(lcs);

cleanup:
    if (obja) decrRefCount(obja);
    if (objb) decrRefCount(objb);
    return;
}

