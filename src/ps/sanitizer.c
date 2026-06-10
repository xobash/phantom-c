#include "phantom/sanitizer.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static bool valid_name(const char*s, size_t max){ if(!s||!*s||strlen(s)>max)return false; for(;*s;s++){ unsigned char c=(unsigned char)*s; if(!(isalnum(c)||c=='_'||c=='-'||c=='.'||c=='$')) return false; } return true; }
bool ph_ensure_service_name(const char *input, char*out,size_t out_len,ph_error*err){ char*t=ph_trim_dup(input); if(!t){ph_error_set(err,1,"allocation failed");return false;} if(!*t){free(t);ph_error_set(err,1,"service name is required");return false;} if(!valid_name(t,128)){free(t);ph_error_set(err,1,"invalid service name");return false;} if(strlen(t)+1>out_len){free(t);ph_error_set(err,1,"service name output too small");return false;} strcpy(out,t); free(t); return true; }
bool ph_safe_package_id(const char*s){ return valid_name(s,200); }
bool ph_safe_feature_name(const char*s){ return valid_name(s,200); }
