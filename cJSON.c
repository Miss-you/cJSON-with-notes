/*
 Copyright (c) 2009 Dave Gamble
 
 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:
 
 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.
 
 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
 */

/* cJSON */
/* JSON parser in C. */

#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <float.h>
#include <limits.h>
#include <ctype.h>
#include "cJSON.h"

/* 什么都不说了。。这个是Error Pointer。。。这名字起的。。 */
static const char *ep;

const char *cJSON_GetErrorPtr(void) {return ep;}

//TODO，没看懂
static int cJSON_strcasecmp(const char *s1,const char *s2)
{
    if (!s1) return (s1==s2)?0:1;if (!s2) return 1;
    for(; tolower(*s1) == tolower(*s2); ++s1, ++s2)	if(*s1 == 0)	return 0;
    return tolower(*(const unsigned char *)s1) - tolower(*(const unsigned char *)s2);
}

//释放
static void *(*cJSON_malloc)(size_t sz) = malloc;
static void (*cJSON_free)(void *ptr) = free;

//拷贝字符串
static char* cJSON_strdup(const char* str)
{
    size_t len;
    char* copy;
    
    len = strlen(str) + 1;
    if (!(copy = (char*)cJSON_malloc(len))) return 0;
    memcpy(copy,str,len);
    return copy;
}

//初始化申请、释放指针（代码风格）
void cJSON_InitHooks(cJSON_Hooks* hooks)
{
    if (!hooks) { /* Reset hooks */
        cJSON_malloc = malloc;
        cJSON_free = free;
        return;
    }
    
    cJSON_malloc = (hooks->malloc_fn)?hooks->malloc_fn:malloc;
    cJSON_free	 = (hooks->free_fn)?hooks->free_fn:free;
}

//创建cJSON节点
/* Internal constructor. */
static cJSON *cJSON_New_Item(void)
{
    cJSON* node = (cJSON*)cJSON_malloc(sizeof(cJSON));
    if (node) memset(node,0,sizeof(cJSON));
    return node;
}

//删除cJSON节点，cJSON节点是一层一层的，所以用迭代删除，优先删除子节点
//代码细节，先保存next，否则会导致异常操作；后面如有类似代码，不重复说明
/* Delete a cJSON structure. */
void cJSON_Delete(cJSON *c)
{
    cJSON *next;
    while (c)
    {
        next=c->next;
        if (!(c->type&cJSON_IsReference) && c->child) cJSON_Delete(c->child);
        if (!(c->type&cJSON_IsReference) && c->valuestring) cJSON_free(c->valuestring);
        if (!(c->type&cJSON_StringIsConst) && c->string) cJSON_free(c->string);
        cJSON_free(c);
        c=next;
    }
}

//cJSON字符串中数字解析，并且把结果存储到item 这个cJSON变量中，返回的是该数字字符串解码完成后的末端指针
/* Parse the input text to generate a number, and populate the result into item. */
static const char *parse_number(cJSON *item,const char *num)
{
    double n=0,sign=1,scale=0;int subscale=0,signsubscale=1;
    
    if (*num=='-') sign=-1,num++;	/* Has sign? */
    if (*num=='0') num++;			/* is zero */
    
    /* 这里的存储方式大概x * 10 ^ n（n可正可负）这样存储
       譬如3，存储为3 * 10 ^ 0
       譬如3.1 存储 31 * 10 ^ -1
       譬如3e10，存储 3 * 10 ^ 10
       当然如果有正负号，还有一个sign/scalesign的值来存储数的正负号和指数的正负号~
    */
    /* 计算到小数点之前或者数字结束 */
    if (*num>='1' && *num<='9')	do	n=(n*10.0)+(*num++ -'0');	while (*num>='0' && *num<='9');	/* Number? */
    /* 如果遇到了小数点？ */
    if (*num=='.' && num[1]>='0' && num[1]<='9') {num++;		do	n=(n*10.0)+(*num++ -'0'),scale--; while (*num>='0' && *num<='9');}	/* Fractional part? */
    /* 如果后面跟的是e，指数 */
    if (*num=='e' || *num=='E')		/* Exponent? */
    {	num++;if (*num=='+') num++;	else if (*num=='-') signsubscale=-1,num++;		/* With sign? */
        while (*num>='0' && *num<='9') subscale=(subscale*10)+(*num++ - '0');	/* Number? */
    }
    
    /* 根据之前的存储方式计算出最后结果 */
    n=sign*n*pow(10.0,(scale+subscale*signsubscale));	/* number = +/- number.fraction * 10^+/- exponent */
    
    /* 计算结果存在double，另外再存储个转化为整数的整数值 */
    item->valuedouble=n;
    item->valueint=(int)n;
    item->type=cJSON_Number;
    return num;
}

//还有一个pow2gt(x) 函数技巧, 返回一个比x大的最小的2的幂，技巧。。。……，比如x=5，那么结果就是8
static int pow2gt (int x)	{	--x;	x|=x>>1;	x|=x>>2;	x|=x>>4;	x|=x>>8;	x|=x>>16;	return x+1;	}

typedef struct {char *buffer; int length; int offset; } printbuffer;

//主要是来保障：1.缓冲区不足时申请新的缓冲区；2.缓冲区足够时，返回缓冲区指针+内容偏移量的值；needed是所需要的缓冲区大小
static char* ensure(printbuffer *p,int needed)
{
    char *newbuffer;int newsize;
    if (!p || !p->buffer) return 0;
    needed+=p->offset;
    if (needed<=p->length) return p->buffer+p->offset;
    
    newsize=pow2gt(needed);
    newbuffer=(char*)cJSON_malloc(newsize);
    if (!newbuffer) {cJSON_free(p->buffer);p->length=0,p->buffer=0;return 0;}//申请空间失败
    if (newbuffer) memcpy(newbuffer,p->buffer,p->length);
    cJSON_free(p->buffer);
    p->length=newsize;
    p->buffer=newbuffer;
    return newbuffer+p->offset;
}

//用于更新printbuffer的offset值更新。返回操作后新的offset值
static int update(printbuffer *p)
{
    char *str;
    if (!p || !p->buffer) return 0;
    str=p->buffer+p->offset;
    return p->offset+strlen(str);
}

//将cJSON格式的数字转，输出至缓冲区
/* Render the number nicely from the given item into a string. */
static char *print_number(cJSON *item,printbuffer *p)
{
    char *str=0;
    double d=item->valuedouble;
    if (d==0)
    {//说明是0
        if (p)	str=ensure(p,2);
        else	str=(char*)cJSON_malloc(2);	/* special case for 0. */
        if (str) strcpy(str,"0");
    }
    else if (fabs(((double)item->valueint)-d)<=DBL_EPSILON && d<=INT_MAX && d>=INT_MIN)
    {//额，认为这个就是个整数……
        if (p)	str=ensure(p,21);
        else	str=(char*)cJSON_malloc(21);	/* 2^64+1 can be represented in 21 chars. */
        if (str)	sprintf(str,"%d",item->valueint);
    }
    else
    {//说明这个是个浮点数……
        if (p)	str=ensure(p,64);
        else	str=(char*)cJSON_malloc(64);	/* This is a nice tradeoff. */
        if (str)
        {
            /*
             int fpclassify(x)  用来查看浮点数x的情况，fpclassify可以用任何浮点数表达式作为参数，fpclassify的返回值有以下几种情况。
             FP_NAN：x是一个“not a number”。
             FP_INFINITE: x是正、负无穷。
             FP_ZERO: x是0。
             FP_SUBNORMAL: x太小，以至于不能用浮点数的规格化形式表示。
             FP_NORMAL: x是一个正常的浮点数（不是以上结果中的任何一种）。
             */
            if (fpclassify(d) != FP_ZERO && !isnormal(d))				sprintf(str,"null");
            //数太小，趋近于整数
            else if (fabs(floor(d)-d)<=DBL_EPSILON && fabs(d)<1.0e60)	sprintf(str,"%.0f",d);
            //数指数位太长，完全打印出来过长，故用科学计数法
            else if (fabs(d)<1.0e-6 || fabs(d)>1.0e9)					sprintf(str,"%e",d);
            //普通浮点数
            else														sprintf(str,"%f",d);
        }
    }
    return str;
}

//处理一个有四个位数的16进制数……，譬如0XAB12，仅仅处理AB12
static unsigned parse_hex4(const char *str)
{
    unsigned h=0;
    if (*str>='0' && *str<='9') h+=(*str)-'0'; else if (*str>='A' && *str<='F') h+=10+(*str)-'A'; else if (*str>='a' && *str<='f') h+=10+(*str)-'a'; else return 0;
    h=h<<4;str++;
    if (*str>='0' && *str<='9') h+=(*str)-'0'; else if (*str>='A' && *str<='F') h+=10+(*str)-'A'; else if (*str>='a' && *str<='f') h+=10+(*str)-'a'; else return 0;
    h=h<<4;str++;
    if (*str>='0' && *str<='9') h+=(*str)-'0'; else if (*str>='A' && *str<='F') h+=10+(*str)-'A'; else if (*str>='a' && *str<='f') h+=10+(*str)-'a'; else return 0;
    h=h<<4;str++;
    if (*str>='0' && *str<='9') h+=(*str)-'0'; else if (*str>='A' && *str<='F') h+=10+(*str)-'A'; else if (*str>='a' && *str<='f') h+=10+(*str)-'a'; else return 0;
    return h;
}

/* Parse the input text into an unescaped cstring, and populate item. */
//不太懂这个作何用
static const unsigned char firstByteMark[7] = { 0x00, 0x00, 0xC0, 0xE0, 0xF0, 0xF8, 0xFC };
static const char *parse_string(cJSON *item,const char *str)
{
    const char *ptr=str+1;char *ptr2;char *out;int len=0;unsigned uc,uc2;
    if (*str!='\"') {ep=str;return 0;}	/* not a string! */
    
    while (*ptr!='\"' && *ptr && ++len) if (*ptr++ == '\\') ptr++;	/* Skip escaped quotes. */
    
    out=(char*)cJSON_malloc(len+1);	/* This is how long we need for the string, roughly. */
    /* alloc memory failed */
    if (!out) return 0;
    
    /* back to string head */
    ptr=str+1;ptr2=out;
    while (*ptr!='\"' && *ptr)
    {
        /* copy */
        if (*ptr!='\\') *ptr2++=*ptr++;
        else
        {
            ptr++;
            switch (*ptr)
            {
                case 'b': *ptr2++='\b';	break;
                case 'f': *ptr2++='\f';	break;
                case 'n': *ptr2++='\n';	break;
                case 'r': *ptr2++='\r';	break;
                case 't': *ptr2++='\t';	break;
                /* 处理unicode字符 */
                case 'u':	 /* transcode utf16 to utf8. */
                    uc=parse_hex4(ptr+1);ptr+=4;	/* get the unicode char. */
                    
                    if ((uc>=0xDC00 && uc<=0xDFFF) || uc==0)	break;	/* check for invalid.	*/
                    
                    if (uc>=0xD800 && uc<=0xDBFF)	/* UTF16 surrogate pairs.	*/
                    {
                        if (ptr[1]!='\\' || ptr[2]!='u')	break;	/* missing second-half of surrogate.	*/
                        uc2=parse_hex4(ptr+3);ptr+=6;
                        if (uc2<0xDC00 || uc2>0xDFFF)		break;	/* invalid second-half of surrogate.	*/
                        uc=0x10000 + (((uc&0x3FF)<<10) | (uc2&0x3FF));
                    }
                    
                    len=4;if (uc<0x80) len=1;else if (uc<0x800) len=2;else if (uc<0x10000) len=3; ptr2+=len;
                    
                    switch (len) {
                        case 4: *--ptr2 =((uc | 0x80) & 0xBF); uc >>= 6;
                        case 3: *--ptr2 =((uc | 0x80) & 0xBF); uc >>= 6;
                        case 2: *--ptr2 =((uc | 0x80) & 0xBF); uc >>= 6;
                        case 1: *--ptr2 =(uc | firstByteMark[len]);
                    }
                    ptr2+=len;
                    break;
                default:  *ptr2++=*ptr; break;
            }
            ptr++;
        }
    }
    *ptr2=0;
    if (*ptr=='\"') ptr++;
    item->valuestring=out;
    item->type=cJSON_String;
    return ptr;
}

/* Render the cstring provided to an escaped version that can be printed. */
static char *print_string_ptr(const char *str,printbuffer *p)
{
    const char *ptr;char *ptr2,*out;int len=0,flag=0;unsigned char token;
    
    if (!str)
    {
        if (p)	out=ensure(p,3);
        else	out=(char*)cJSON_malloc(3);
        if (!out) return 0;
        strcpy(out,"\"\"");
        return out;
    }
    
    for (ptr=str;*ptr;ptr++) flag|=((*ptr>0 && *ptr<32)||(*ptr=='\"')||(*ptr=='\\'))?1:0;
    if (!flag)
    {
        len=ptr-str;
        if (p) out=ensure(p,len+3);
        else		out=(char*)cJSON_malloc(len+3);
        if (!out) return 0;
        ptr2=out;*ptr2++='\"';
        strcpy(ptr2,str);
        ptr2[len]='\"';
        ptr2[len+1]=0;
        return out;
    }
    
    ptr=str;while ((token=*ptr) && ++len) {if (strchr("\"\\\b\f\n\r\t",token)) len++; else if (token<32) len+=5;ptr++;}
    
    if (p)	out=ensure(p,len+3);
    else	out=(char*)cJSON_malloc(len+3);
    if (!out) return 0;
    
    ptr2=out;ptr=str;
    *ptr2++='\"';
    while (*ptr)
    {
        if ((unsigned char)*ptr>31 && *ptr!='\"' && *ptr!='\\') *ptr2++=*ptr++;
        else
        {
            *ptr2++='\\';
            switch (token=*ptr++)
            {
                case '\\':	*ptr2++='\\';	break;
                case '\"':	*ptr2++='\"';	break;
                case '\b':	*ptr2++='b';	break;
                case '\f':	*ptr2++='f';	break;
                case '\n':	*ptr2++='n';	break;
                case '\r':	*ptr2++='r';	break;
                case '\t':	*ptr2++='t';	break;
                default: sprintf(ptr2,"u%04x",token);ptr2+=5;	break;	/* escape and print */
            }
        }
    }
    *ptr2++='\"';*ptr2++=0;
    return out;
}
/* Invote print_string_ptr (which is useful) on an item. */
static char *print_string(cJSON *item,printbuffer *p)	{return print_string_ptr(item->valuestring,p);}

/* Predeclare these prototypes. */
static const char *parse_value(cJSON *item,const char *value);
static char *print_value(cJSON *item,int depth,int fmt,printbuffer *p);
static const char *parse_array(cJSON *item,const char *value);
static char *print_array(cJSON *item,int depth,int fmt,printbuffer *p);
static const char *parse_object(cJSON *item,const char *value);
static char *print_object(cJSON *item,int depth,int fmt,printbuffer *p);

/* Utility to jump whitespace and cr/lf */
static const char *skip(const char *in) {while (in && *in && (unsigned char)*in<=32) in++; return in;}

/* 创建cJSON结构体根节点 */
/* Parse an object - create a new root, and populate. */
cJSON *cJSON_ParseWithOpts(const char *value,const char **return_parse_end,int require_null_terminated)
{
    const char *end=0;
    cJSON *c=cJSON_New_Item();
    ep=0;
    if (!c) return 0;       /* memory fail */
    
    end=parse_value(c,skip(value));
    if (!end)	{cJSON_Delete(c);return 0;}	/* parse failure. ep is set. */
    
    /* if we require null-terminated JSON without appended garbage, skip and then check for a null terminator */
    if (require_null_terminated) {end=skip(end);if (*end) {cJSON_Delete(c);ep=end;return 0;}}
    if (return_parse_end) *return_parse_end=end;
    return c;
}
/* Default options for cJSON_Parse */
cJSON *cJSON_Parse(const char *value) {return cJSON_ParseWithOpts(value,0,0);}

/* 将cJSON对象转换成字符串 */
/* Render a cJSON item/entity/structure to text. */
char *cJSON_Print(cJSON *item)				{return print_value(item,0,1,0);}
char *cJSON_PrintUnformatted(cJSON *item)	{return print_value(item,0,0,0);}

char *cJSON_PrintBuffered(cJSON *item,int prebuffer,int fmt)
{
    printbuffer p;
    p.buffer=(char*)cJSON_malloc(prebuffer);
    p.length=prebuffer;
    p.offset=0;
    return print_value(item,0,fmt,&p);
}


/* Parser core - when encountering text, process appropriately. */
static const char *parse_value(cJSON *item,const char *value)
{
    if (!value)						return 0;	/* Fail on null. */
    if (!strncmp(value,"null",4))	{ item->type=cJSON_NULL;  return value+4; }
    if (!strncmp(value,"false",5))	{ item->type=cJSON_False; return value+5; }
    if (!strncmp(value,"true",4))	{ item->type=cJSON_True; item->valueint=1;	return value+4; }
    /* string数据 */
    if (*value=='\"')				{ return parse_string(item,value); }
    /* 数值数据 */
    if (*value=='-' || (*value>='0' && *value<='9'))	{ return parse_number(item,value); }
    /* JSON对象数组 */
    if (*value=='[')				{ return parse_array(item,value); }
    /* JSON对象 */
    if (*value=='{')				{ return parse_object(item,value); }
    
    ep=value;return 0;	/* failure. */
}

/* Render a value to text. */
static char *print_value(cJSON *item,int depth,int fmt,printbuffer *p)
{
    char *out=0;
    if (!item) return 0;
    if (p)
    {
        switch ((item->type)&255)
        {
            /* 先保证有足够字符空间，然后拷贝 */
            case cJSON_NULL:	{out=ensure(p,5);	if (out) strcpy(out,"null");	break;}
            case cJSON_False:	{out=ensure(p,6);	if (out) strcpy(out,"false");	break;}
            case cJSON_True:	{out=ensure(p,5);	if (out) strcpy(out,"true");	break;}
            case cJSON_Number:	out=print_number(item,p);break;
            case cJSON_String:	out=print_string(item,p);break;
            case cJSON_Array:	out=print_array(item,depth,fmt,p);break;
            case cJSON_Object:	out=print_object(item,depth,fmt,p);break;
        }
    }
    else
    {
        /* 创建空间 */
        switch ((item->type)&255)
        {
            case cJSON_NULL:	out=cJSON_strdup("null");	break;
            case cJSON_False:	out=cJSON_strdup("false");break;
            case cJSON_True:	out=cJSON_strdup("true"); break;
            case cJSON_Number:	out=print_number(item,0);break;
            case cJSON_String:	out=print_string(item,0);break;
            case cJSON_Array:	out=print_array(item,depth,fmt,0);break;
            case cJSON_Object:	out=print_object(item,depth,fmt,0);break;
        }
    }
    return out;
}

/* Build an array from input text. */
static const char *parse_array(cJSON *item,const char *value)
{
    /* 
        整体思路就是，到]之前，循环调用parse_value来处理数值，注意一些具体细节、循环结束条件以及跳过空格等字符
    */
    cJSON *child;
    if (*value!='[')	{ep=value;return 0;}	/* not an array! */
    
    item->type=cJSON_Array;
    value=skip(value+1);
    if (*value==']') return value+1;	/* empty array. */
    
    item->child=child=cJSON_New_Item();
    if (!item->child) return 0;		 /* memory fail */
    
    /*  */
    value=skip(parse_value(child,skip(value)));	/* skip any spacing, get the value. */
    if (!value) return 0;
    
    while (*value==',')
    {
        cJSON *new_item;
        if (!(new_item=cJSON_New_Item())) return 0; 	/* memory fail */
        child->next=new_item;new_item->prev=child;child=new_item;
        value=skip(parse_value(child,skip(value+1)));
        if (!value) return 0;	/* memory fail */
    }
    
    if (*value==']') return value+1;	/* end of array */
    ep=value;return 0;	/* malformed. */
}

/* Render an array to text */
static char *print_array(cJSON *item,int depth,int fmt,printbuffer *p)
{
    char **entries;
    char *out=0,*ptr,*ret;int len=5;
    cJSON *child=item->child;
    int numentries=0,i=0,fail=0;
    size_t tmplen=0;
    
    /* How many entries in the array? */
    while (child) numentries++,child=child->next;
    /* Explicitly handle numentries==0 */
    if (!numentries)
    {
        if (p)	out=ensure(p,3);
        else	out=(char*)cJSON_malloc(3);
        if (out) strcpy(out,"[]");
        return out;
    }
    
    if (p)
    {
        /* Compose the output array. */
        i=p->offset;
        ptr=ensure(p,1);if (!ptr) return 0;	*ptr='[';	p->offset++;
        child=item->child;
        while (child && !fail)
        {
            print_value(child,depth+1,fmt,p);
            p->offset=update(p);
            if (child->next) {len=fmt?2:1;ptr=ensure(p,len+1);if (!ptr) return 0;*ptr++=',';if(fmt)*ptr++=' ';*ptr=0;p->offset+=len;}
            child=child->next;
        }
        ptr=ensure(p,2);if (!ptr) return 0;	*ptr++=']';*ptr=0;
        out=(p->buffer)+i;
    }
    else
    {
        /* Allocate an array to hold the values for each */
        entries=(char**)cJSON_malloc(numentries*sizeof(char*));
        if (!entries) return 0;
        memset(entries,0,numentries*sizeof(char*));
        /* Retrieve all the results: */
        child=item->child;
        while (child && !fail)
        {
            ret=print_value(child,depth+1,fmt,0);
            entries[i++]=ret;
            if (ret) len+=strlen(ret)+2+(fmt?1:0); else fail=1;
            child=child->next;
        }
        
        /* If we didn't fail, try to malloc the output string */
        if (!fail)	out=(char*)cJSON_malloc(len);
        /* If that fails, we fail. */
        if (!out) fail=1;
        
        /* Handle failure. */
        if (fail)
        {
            for (i=0;i<numentries;i++) if (entries[i]) cJSON_free(entries[i]);
            cJSON_free(entries);
            return 0;
        }
        
        /* Compose the output array. */
        *out='[';
        ptr=out+1;*ptr=0;
        for (i=0;i<numentries;i++)
        {
            tmplen=strlen(entries[i]);memcpy(ptr,entries[i],tmplen);ptr+=tmplen;
            if (i!=numentries-1) {*ptr++=',';if(fmt)*ptr++=' ';*ptr=0;}
            cJSON_free(entries[i]);
        }
        cJSON_free(entries);
        *ptr++=']';*ptr++=0;
    }
    return out;
}

/* Build an object from the text. */
/* 处理JSON 对象，同一层中的所有JSON对象都会处理
   因为JSON对象结构就是{{vala:vala1}, {valb:valb1} ... }
   那么该函数的作用就是读取vala、valb的值，然后处理vala1、valb1
   处理方式跟array差不多……
*/
static const char *parse_object(cJSON *item,const char *value)
{
    cJSON *child;
    if (*value!='{')	{ep=value;return 0;}	/* not an object! */
    
    item->type=cJSON_Object;
    value=skip(value+1);
    if (*value=='}') return value+1;	/* empty array. */
    
    item->child=child=cJSON_New_Item();
    if (!item->child) return 0;
    /* 先跳过空格，然后读取键值，然后再跳空格 */
    value=skip(parse_string(child,skip(value)));
    if (!value) return 0;
    child->string=child->valuestring;child->valuestring=0;
    /* 处理到':'了 */
    if (*value!=':') {ep=value;return 0;}	/* fail! */
    /* 处理键值所存储的值，这个值有可能是Number、balba什么都有可能 */
    value=skip(parse_value(child,skip(value+1)));	/* skip any spacing, get the value. */
    if (!value) return 0;
    
    /* 循环处理 */
    while (*value==',')
    {
        cJSON *new_item;
        if (!(new_item=cJSON_New_Item()))	return 0; /* memory fail */
        child->next=new_item;new_item->prev=child;child=new_item;
        value=skip(parse_string(child,skip(value+1)));
        if (!value) return 0;
        child->string=child->valuestring;child->valuestring=0;
        if (*value!=':') {ep=value;return 0;}	/* fail! */
        value=skip(parse_value(child,skip(value+1)));	/* skip any spacing, get the value. */
        if (!value) return 0;
    }
    
    if (*value=='}') return value+1;	/* end of array */
    ep=value;return 0;	/* malformed. */
}

/* Render an object to text. */
static char *print_object(cJSON *item,int depth,int fmt,printbuffer *p)
{
    char **entries=0,**names=0;
    char *out=0,*ptr,*ret,*str;int len=7,i=0,j;
    cJSON *child=item->child;
    int numentries=0,fail=0;
    size_t tmplen=0;
    /* Count the number of entries. */
    while (child) numentries++,child=child->next;
    /* Explicitly handle empty object case */
    if (!numentries)
    {
        if (p) out=ensure(p,fmt?depth+4:3);
        else	out=(char*)cJSON_malloc(fmt?depth+4:3);
        if (!out)	return 0;
        ptr=out;*ptr++='{';
        if (fmt) {*ptr++='\n';for (i=0;i<depth;i++) *ptr++='\t';}
        *ptr++='}';*ptr++=0;
        return out;
    }
    /* 如果已经申请空间 */
    if (p)
    {
        /* Compose the output: */
        i=p->offset;
        len=fmt?2:1;	ptr=ensure(p,len+1);	if (!ptr) return 0;
        *ptr++='{';	if (fmt) *ptr++='\n';	*ptr=0;	p->offset+=len;
        child=item->child;depth++;
        while (child)
        {
            if (fmt)
            {
                ptr=ensure(p,depth);	if (!ptr) return 0;
                for (j=0;j<depth;j++) *ptr++='\t';
                p->offset+=depth;
            }
            print_string_ptr(child->string,p);
            p->offset=update(p);
            
            len=fmt?2:1;
            ptr=ensure(p,len);	if (!ptr) return 0;
            *ptr++=':';if (fmt) *ptr++='\t';
            p->offset+=len;
            
            print_value(child,depth,fmt,p);
            p->offset=update(p);
            
            len=(fmt?1:0)+(child->next?1:0);
            ptr=ensure(p,len+1); if (!ptr) return 0;
            if (child->next) *ptr++=',';
            if (fmt) *ptr++='\n';*ptr=0;
            p->offset+=len;
            child=child->next;
        }
        ptr=ensure(p,fmt?(depth+1):2);	 if (!ptr) return 0;
        if (fmt)	for (i=0;i<depth-1;i++) *ptr++='\t';
        *ptr++='}';*ptr=0;
        out=(p->buffer)+i;
    }
    /* 如果还未申请空间~ */
    else
    {
        /* Allocate space for the names and the objects */
        entries=(char**)cJSON_malloc(numentries*sizeof(char*));
        if (!entries) return 0;
        names=(char**)cJSON_malloc(numentries*sizeof(char*));
        if (!names) {cJSON_free(entries);return 0;}
        memset(entries,0,sizeof(char*)*numentries);
        memset(names,0,sizeof(char*)*numentries);
        
        /* Collect all the results into our arrays: */
        child=item->child;depth++;if (fmt) len+=depth;
        while (child && !fail)
        {
            names[i]=str=print_string_ptr(child->string,0);
            entries[i++]=ret=print_value(child,depth,fmt,0);
            if (str && ret) len+=strlen(ret)+strlen(str)+2+(fmt?2+depth:0); else fail=1;
            child=child->next;
        }
        
        /* Try to allocate the output string */
        if (!fail)	out=(char*)cJSON_malloc(len);
        if (!out) fail=1;
        
        /* Handle failure */
        if (fail)
        {
            for (i=0;i<numentries;i++) {if (names[i]) cJSON_free(names[i]);if (entries[i]) cJSON_free(entries[i]);}
            cJSON_free(names);cJSON_free(entries);
            return 0;
        }
        
        /* Compose the output: */
        *out='{';ptr=out+1;if (fmt)*ptr++='\n';*ptr=0;
        for (i=0;i<numentries;i++)
        {
            if (fmt) for (j=0;j<depth;j++) *ptr++='\t';
            tmplen=strlen(names[i]);memcpy(ptr,names[i],tmplen);ptr+=tmplen;
            *ptr++=':';if (fmt) *ptr++='\t';
            strcpy(ptr,entries[i]);ptr+=strlen(entries[i]);
            if (i!=numentries-1) *ptr++=',';
            if (fmt) *ptr++='\n';*ptr=0;
            cJSON_free(names[i]);cJSON_free(entries[i]);
        }
        
        cJSON_free(names);cJSON_free(entries);
        if (fmt) for (i=0;i<depth-1;i++) *ptr++='\t';
        *ptr++='}';*ptr++=0;
    }
    return out;
}

/* Get Array size/item / object item. */
/* array是链表存储，next指向下一个，所以直接遍历next指针就可以知道array大小 */
int    cJSON_GetArraySize(cJSON *array)							{cJSON *c=array->child;int i=0;while(c)i++,c=c->next;return i;}
/* 跟cJSON_GetArraySize差不多，增加个计数即可 */
cJSON *cJSON_GetArrayItem(cJSON *array,int item)				{cJSON *c; if (array == NULL) return NULL; c=array->child;  while (c && item>0) item--,c=c->next; return c;}
/* 遍历当前JSON数组，找出键值为string的JSON对象，否则返回NULL */
cJSON *cJSON_GetObjectItem(cJSON *object,const char *string)	{cJSON *c; if (object == NULL) return NULL; c=object->child; while (c && cJSON_strcasecmp(c->string,string)) c=c->next; return c;}
/* 遍历当前JSON数组，查询是否有键值为string的JSON对象，有返回1，没有返回0（C语言中0代表false，非0代表true） */
int cJSON_HasObjectItem(cJSON *object,const char *string)	{
    cJSON *c=object->child;
    while (c )
    {
        if(cJSON_strcasecmp(c->string,string)==0){
            return 1;
        }
        c=c->next;
    }
    return 0;
}

/* Utility for array list handling. */
static void suffix_object(cJSON *prev,cJSON *item) {prev->next=item;item->prev=prev;}
/* Utility for handling references. */
static cJSON *create_reference(cJSON *item) {cJSON *ref=cJSON_New_Item();if (!ref) return 0;memcpy(ref,item,sizeof(cJSON));ref->string=0;ref->type|=cJSON_IsReference;ref->next=ref->prev=0;return ref;}

/* Add item to array/object. */
/* 如果array没有对象，直接添加；如果有对象，则直接添加到末尾 */
void   cJSON_AddItemToArray(cJSON *array, cJSON *item)						{cJSON *c=array->child;if (!item) return; if (!c) {array->child=item;} else {while (c && c->next) c=c->next; suffix_object(c,item);}}
/* 。。。这个有点神奇，，就是在JSON数组中添加键值为string、JSON内容为item的JSON对象 */
void   cJSON_AddItemToObject(cJSON *object,const char *string,cJSON *item)	{if (!item) return; if (item->string) cJSON_free(item->string);item->string=cJSON_strdup(string);cJSON_AddItemToArray(object,item);}
/* ？？？？不懂 */
void   cJSON_AddItemToObjectCS(cJSON *object,const char *string,cJSON *item)	{if (!item) return; if (!(item->type&cJSON_StringIsConst) && item->string) cJSON_free(item->string);item->string=(char*)string;item->type|=cJSON_StringIsConst;cJSON_AddItemToArray(object,item);}
void	cJSON_AddItemReferenceToArray(cJSON *array, cJSON *item)						{cJSON_AddItemToArray(array,create_reference(item));}
void	cJSON_AddItemReferenceToObject(cJSON *object,const char *string,cJSON *item)	{cJSON_AddItemToObject(object,string,create_reference(item));}

cJSON *cJSON_DetachItemFromArray(cJSON *array,int which)			{cJSON *c=array->child;while (c && which>0) c=c->next,which--;if (!c) return 0;
    if (c->prev) c->prev->next=c->next;if (c->next) c->next->prev=c->prev;if (c==array->child) array->child=c->next;c->prev=c->next=0;return c;}
void   cJSON_DeleteItemFromArray(cJSON *array,int which)			{cJSON_Delete(cJSON_DetachItemFromArray(array,which));}
cJSON *cJSON_DetachItemFromObject(cJSON *object,const char *string) {int i=0;cJSON *c=object->child;while (c && cJSON_strcasecmp(c->string,string)) i++,c=c->next;if (c) return cJSON_DetachItemFromArray(object,i);return 0;}
void   cJSON_DeleteItemFromObject(cJSON *object,const char *string) {cJSON_Delete(cJSON_DetachItemFromObject(object,string));}

/* Replace array/object items with new ones. */
void   cJSON_InsertItemInArray(cJSON *array,int which,cJSON *newitem)		{cJSON *c=array->child;while (c && which>0) c=c->next,which--;if (!c) {cJSON_AddItemToArray(array,newitem);return;}
    newitem->next=c;newitem->prev=c->prev;c->prev=newitem;if (c==array->child) array->child=newitem; else newitem->prev->next=newitem;}
void   cJSON_ReplaceItemInArray(cJSON *array,int which,cJSON *newitem)		{cJSON *c=array->child;while (c && which>0) c=c->next,which--;if (!c) return;
    newitem->next=c->next;newitem->prev=c->prev;if (newitem->next) newitem->next->prev=newitem;
    if (c==array->child) array->child=newitem; else newitem->prev->next=newitem;c->next=c->prev=0;cJSON_Delete(c);}
void   cJSON_ReplaceItemInObject(cJSON *object,const char *string,cJSON *newitem){int i=0;cJSON *c=object->child;while(c && cJSON_strcasecmp(c->string,string))i++,c=c->next;if(c){newitem->string=cJSON_strdup(string);cJSON_ReplaceItemInArray(object,i,newitem);}}

/* Create basic types: */
/* 没啥好说的，很简单 */
cJSON *cJSON_CreateNull(void)					{cJSON *item=cJSON_New_Item();if(item)item->type=cJSON_NULL;return item;}
cJSON *cJSON_CreateTrue(void)					{cJSON *item=cJSON_New_Item();if(item)item->type=cJSON_True;return item;}
cJSON *cJSON_CreateFalse(void)					{cJSON *item=cJSON_New_Item();if(item)item->type=cJSON_False;return item;}
cJSON *cJSON_CreateBool(int b)					{cJSON *item=cJSON_New_Item();if(item)item->type=b?cJSON_True:cJSON_False;return item;}
cJSON *cJSON_CreateNumber(double num)			{cJSON *item=cJSON_New_Item();if(item){item->type=cJSON_Number;item->valuedouble=num;item->valueint=(int)num;}return item;}
cJSON *cJSON_CreateString(const char *string)	{cJSON *item=cJSON_New_Item();if(item){item->type=cJSON_String;item->valuestring=cJSON_strdup(string);}return item;}
cJSON *cJSON_CreateArray(void)					{cJSON *item=cJSON_New_Item();if(item)item->type=cJSON_Array;return item;}
cJSON *cJSON_CreateObject(void)					{cJSON *item=cJSON_New_Item();if(item)item->type=cJSON_Object;return item;}

/* Create Arrays: */
/* 没啥好说的，很简单 */
cJSON *cJSON_CreateIntArray(const int *numbers,int count)		{int i;cJSON *n=0,*p=0,*a=cJSON_CreateArray();for(i=0;a && i<count;i++){n=cJSON_CreateNumber(numbers[i]);if(!i)a->child=n;else suffix_object(p,n);p=n;}return a;}
cJSON *cJSON_CreateFloatArray(const float *numbers,int count)	{int i;cJSON *n=0,*p=0,*a=cJSON_CreateArray();for(i=0;a && i<count;i++){n=cJSON_CreateNumber(numbers[i]);if(!i)a->child=n;else suffix_object(p,n);p=n;}return a;}
cJSON *cJSON_CreateDoubleArray(const double *numbers,int count)	{int i;cJSON *n=0,*p=0,*a=cJSON_CreateArray();for(i=0;a && i<count;i++){n=cJSON_CreateNumber(numbers[i]);if(!i)a->child=n;else suffix_object(p,n);p=n;}return a;}
cJSON *cJSON_CreateStringArray(const char **strings,int count)	{int i;cJSON *n=0,*p=0,*a=cJSON_CreateArray();for(i=0;a && i<count;i++){n=cJSON_CreateString(strings[i]);if(!i)a->child=n;else suffix_object(p,n);p=n;}return a;}

/* Duplication */
cJSON *cJSON_Duplicate(cJSON *item,int recurse)
{
    cJSON *newitem,*cptr,*nptr=0,*newchild;
    /* Bail on bad ptr */
    if (!item) return 0;
    /* Create new item */
    newitem=cJSON_New_Item();
    if (!newitem) return 0;
    /* Copy over all vars */
    newitem->type=item->type&(~cJSON_IsReference),newitem->valueint=item->valueint,newitem->valuedouble=item->valuedouble;
    if (item->valuestring)	{newitem->valuestring=cJSON_strdup(item->valuestring);	if (!newitem->valuestring)	{cJSON_Delete(newitem);return 0;}}
    if (item->string)		{newitem->string=cJSON_strdup(item->string);			if (!newitem->string)		{cJSON_Delete(newitem);return 0;}}
    /* If non-recursive, then we're done! */
    if (!recurse) return newitem;
    /* Walk the ->next chain for the child. */
    cptr=item->child;
    while (cptr)
    {
        newchild=cJSON_Duplicate(cptr,1);		/* Duplicate (with recurse) each item in the ->next chain */
        if (!newchild) {cJSON_Delete(newitem);return 0;}
        if (nptr)	{nptr->next=newchild,newchild->prev=nptr;nptr=newchild;}	/* If newitem->child already set, then crosswire ->prev and ->next and move on */
        else		{newitem->child=newchild;nptr=newchild;}					/* Set newitem->child and move to it */
        cptr=cptr->next;
    }
    return newitem;
}

void cJSON_Minify(char *json)
{
    char *into=json;
    while (*json)
    {
        if (*json==' ') json++;
        else if (*json=='\t') json++;	/* Whitespace characters. */
        else if (*json=='\r') json++;
        else if (*json=='\n') json++;
        else if (*json=='/' && json[1]=='/')  while (*json && *json!='\n') json++;	/* double-slash comments, to end of line. */
        else if (*json=='/' && json[1]=='*') {while (*json && !(*json=='*' && json[1]=='/')) json++;json+=2;}	/* multiline comments. */
        else if (*json=='\"'){*into++=*json++;while (*json && *json!='\"'){if (*json=='\\') *into++=*json++;*into++=*json++;}*into++=*json++;} /* string literals, which are \" sensitive. */
        else *into++=*json++;			/* All other characters. */
    }
    *into=0;	/* and null-terminate. */
}
