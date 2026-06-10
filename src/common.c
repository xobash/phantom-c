#include "phantom/common.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void ph_error_set(ph_error *err, int code, const char *fmt, ...){ if(!err)return; err->code=code; va_list ap; va_start(ap,fmt); vsnprintf(err->message,sizeof err->message,fmt,ap); va_end(ap); }
char *ph_strdup(const char *s){ if(!s)s=""; size_t n=strlen(s)+1; char *p=(char*)malloc(n); if(p)memcpy(p,s,n); return p; }
char *ph_trim_dup(const char *s){ if(!s)return ph_strdup(""); const unsigned char *b=(const unsigned char*)s; while(*b && isspace(*b))b++; const unsigned char *e=(const unsigned char*)s+strlen(s); while(e>b && isspace(e[-1]))e--; size_t n=(size_t)(e-b); char *p=(char*)malloc(n+1); if(!p)return NULL; memcpy(p,b,n); p[n]=0; return p; }
bool ph_streqi(const char *a,const char*b){ if(!a||!b)return a==b; while(*a&&*b){ if(tolower((unsigned char)*a)!=tolower((unsigned char)*b))return false; a++;b++; } return *a==*b; }
bool ph_starts_i(const char*s,const char*p){ if(!s||!p)return false; while(*p){ if(!*s||tolower((unsigned char)*s)!=tolower((unsigned char)*p))return false; s++;p++; } return true; }
bool ph_ends_i(const char*s,const char*suf){ if(!s||!suf)return false; size_t n=strlen(s),m=strlen(suf); if(m>n)return false; return ph_streqi(s+n-m,suf); }
bool ph_contains_i(const char *h,const char*n){ if(!h||!n)return false; size_t nl=strlen(n); if(nl==0)return true; for(;*h;h++){ size_t i=0; while(i<nl && h[i] && tolower((unsigned char)h[i])==tolower((unsigned char)n[i]))i++; if(i==nl)return true; } return false; }
void ph_slash_normalize(char*s){ if(!s)return; for(;*s;s++) if(*s=='\\') *s='/'; }
