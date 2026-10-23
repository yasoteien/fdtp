// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
/* Part of archive.o (included from archive.c): the archive model and the
 * listings. tar is read from `tar -tvf`; zip and LHa headers are read here
 * (unzip and lhasa replace non-ASCII names with '?' in their listings). */

typedef enum { FMT_NONE, FMT_TAR, FMT_ZIP, FMT_LHA, FMT_7Z } ArchiveFormat;

typedef struct {
    char *raw;   /* the name as stored: given back to the extracting command */
    char *path;  /* UTF-8, '/'-separated, no "." parts, no trailing '/' */
    char *link;  /* symbolic or hard link target, or NULL */
    uint64_t size;
    time_t mtime;
    unsigned long mode;
    int is_dir, is_link, hard, unsafe;
} Member;

typedef struct {
    char *path; /* absolute archive path */
    ArchiveFormat format;
    Member *members;
    size_t count, cap;
    size_t *links, link_count; /* indices of the symbolic links */
} Archive;

static int ends_with(const char *s,const char *suffix)
{
    size_t a=strlen(s),b=strlen(suffix);
    return a>=b && !strcasecmp(s+a-b,suffix);
}

static ArchiveFormat archive_format(const char *path)
{
    static const char *const tar[]={".tar",".tar.gz",".tgz",".tar.bz2",".tbz",".tbz2",".tar.xz",".txz",".tar.z",".taz"};
    for(size_t i=0;i<sizeof(tar)/sizeof(*tar);++i) if(ends_with(path,tar[i])) return FMT_TAR;
    if(ends_with(path,".zip")) return FMT_ZIP;
    if(ends_with(path,".lzh") || ends_with(path,".lha")) return FMT_LHA;
    if(ends_with(path,".7z")) return FMT_7Z;
    return FMT_NONE;
}

static void archive_free(Archive *a)
{
    for(size_t i=0;i<a->count;++i) { free(a->members[i].raw); free(a->members[i].path); free(a->members[i].link); }
    free(a->members); free(a->links); free(a->path); memset(a,0,sizeof(*a));
}

/* Names that are not UTF-8 are shown as Shift_JIS (the usual zip/LHa case). */
static char *display_utf8(const char *raw)
{
    size_t n=strlen(raw); char *out=NULL; size_t len;
    JisEncoding e=jis_detect((const unsigned char *)raw,n);
    if(e==JIS_UTF8) return strdup(raw);
    if(jis_to_utf8((const unsigned char *)raw,n,e,1,&out,&len)<0) return NULL;
    return out;
}

/* Normalise a member name. unsafe: absolute, a drive, or a ".." part, so it
 * would be written outside the destination. backslash: also a separator. */
static char *normalize_name(const char *name,int backslash,int *unsafe)
{
    char *shown=display_utf8(name); if(!shown) return NULL;
    if(backslash) for(char *p=shown;*p;++p) if(*p=='\\') *p='/';
    *unsafe=shown[0]=='/' || (backslash && ((shown[0]&&shown[1]==':')));
    char *out=malloc(strlen(shown)+1); if(!out) { free(shown); return NULL; }
    size_t o=0; char *save=NULL;
    for(char *part=strtok_r(shown,"/",&save);part;part=strtok_r(NULL,"/",&save)) {
        if(!strcmp(part,".")) continue;
        if(!strcmp(part,"..")) *unsafe=1;
        if(o) out[o++]='/';
        size_t n=strlen(part); memcpy(out+o,part,n); o+=n;
    }
    out[o]=0; free(shown); return out;
}

static Member *add_member(Archive *a,const char *raw,int backslash)
{
    if(a->count==a->cap) {
        size_t cap=a->cap ? a->cap*2 : 64; Member *m=realloc(a->members,cap*sizeof(*m));
        if(!m) return NULL;
        a->members=m; a->cap=cap;
    }
    Member *m=&a->members[a->count]; memset(m,0,sizeof(*m));
    m->raw=strdup(raw); m->path=normalize_name(raw,backslash,&m->unsafe);
    if(!m->raw || !m->path) { free(m->raw); free(m->path); return NULL; }
    if(!*m->path && !m->unsafe) { free(m->raw); free(m->path); return m; } /* "./": the root itself */
    ++a->count; return m;
}

/* ---- tar ---- */

/* GNU tar and bsdtar escape unprintable bytes as \ooo (LC_ALL=C). */
static void unescape(char *s)
{
    char *o=s;
    for(char *p=s;*p;) {
        if(*p!='\\' || !p[1]) { *o++=*p++; continue; }
        ++p;
        if(*p>='0' && *p<='7') { int v=0; for(int i=0;i<3 && *p>='0' && *p<='7';++i) v=v*8+(*p++-'0'); *o++=(char)v; continue; }
        switch(*p) { case 'n': *o++='\n'; break; case 't': *o++='\t'; break; case 'r': *o++='\r'; break;
            case 'a': *o++='\a'; break; case 'b': *o++='\b'; break; case 'f': *o++='\f'; break; case 'v': *o++='\v'; break;
            default: *o++=*p; }
        ++p;
    }
    *o=0;
}

static int month_of(const char *s)
{
    static const char *const m[]={"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
    for(int i=0;i<12;++i) if(!strncmp(s,m[i],3)) return i;
    return -1;
}

static unsigned long parse_mode(const char *t)
{
    unsigned long mode=0; static const char bits[]="rwxrwxrwx";
    for(int i=0;i<9 && t[i+1];++i) if(t[i+1]==bits[i] || t[i+1]=='s' || t[i+1]=='t') mode|=1ul<<(8-i);
    return mode;
}

/* GNU:    mode owner/group size YYYY-MM-DD HH:MM name
 * bsdtar: mode links owner group size Mon DD HH:MM|YYYY name
 * The name follows the last field after exactly one space. */
static int parse_tar_line(Archive *a,char *line)
{
    char tok[8][64]; size_t ntok=0,need=8; char *p=line;
    while(ntok<need) {
        while(*p==' ') ++p;
        if(!*p) return 0;
        char *start=p; while(*p && *p!=' ') ++p;
        size_t n=(size_t)(p-start); if(n>=sizeof(tok[0])) n=sizeof(tok[0])-1;
        memcpy(tok[ntok],start,n); tok[ntok++][n]=0;
        if(ntok==2 && strchr(tok[1],'/')) need=5;
    }
    if(*p!=' ' || !p[1]) return 0;
    int gnu=need==5; char *name=p+1,type=tok[0][0],*link=NULL;
    const char *size=tok[gnu ? 2 : 4];
    if(type=='l') { char *arrow=strstr(name," -> "); if(arrow) { *arrow=0; link=arrow+4; } }
    else if(type=='h' || (type=='-' && !strcmp(size,"0"))) {
        char *to=strstr(name," link to "); if(to) { *to=0; link=to+9; type='h'; }
    }
    unescape(name); if(link) unescape(link);
    if(!*name) return 0;
    int slash=name[strlen(name)-1]=='/';
    Member *m=add_member(a,name,0); if(!m) return -1;
    if(m==&a->members[a->count]) return 0; /* the root entry, dropped */
    m->is_dir=type=='d' || slash;
    m->is_link=type=='l'; m->hard=type=='h'; m->mode=parse_mode(tok[0]);
    if(link && !(m->link=strdup(link))) return -1;
    m->size=strtoull(size,NULL,10);
    struct tm tm={0}; tm.tm_isdst=-1;
    if(gnu) {
        if(sscanf(tok[3],"%d-%d-%d",&tm.tm_year,&tm.tm_mon,&tm.tm_mday)==3) { tm.tm_year-=1900; tm.tm_mon-=1; sscanf(tok[4],"%d:%d",&tm.tm_hour,&tm.tm_min); m->mtime=mktime(&tm); }
    } else if((tm.tm_mon=month_of(tok[5]))>=0) {
        tm.tm_mday=atoi(tok[6]);
        time_t now=time(NULL); struct tm *lt=localtime(&now); int year=lt ? lt->tm_year : 70;
        if(strchr(tok[7],':')) {
            sscanf(tok[7],"%d:%d",&tm.tm_hour,&tm.tm_min); tm.tm_year=year;
            struct tm probe=tm; if(mktime(&probe)>now+86400) --tm.tm_year;
        } else tm.tm_year=atoi(tok[7])-1900;
        m->mtime=mktime(&tm);
    }
    return 0;
}

static int list_tar(Archive *a,char *err,size_t errn)
{
    const char *argv[]={"tar","-tvf",a->path,NULL}; RunResult r;
    if(run_command(argv,NULL,&r)<0) { snprintf(err,errn,"%s",errno==ENOENT ? T("tar がありません") : T("tar を起動できません")); return -1; }
    if(r.status) { char line[160]; first_line(&r,line,sizeof(line)); snprintf(err,errn,T("アーカイブを読めません: %.120s"),line); run_free(&r); return -1; }
    int result=0; char *save=NULL;
    for(char *line=strtok_r(r.out,"\n",&save);line && !result;line=strtok_r(NULL,"\n",&save)) {
        size_t n=strlen(line); if(n && line[n-1]=='\r') line[n-1]=0;
        if(parse_tar_line(a,line)<0) { snprintf(err,errn,"%s",T("メモリが足りません")); result=-1; }
    }
    run_free(&r); return result;
}

/* ---- zip (central directory only; unzip extracts) ---- */

static unsigned u16(const unsigned char *p) { return p[0]|(unsigned)p[1]<<8; }
static uint32_t u32(const unsigned char *p) { return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static uint64_t u64(const unsigned char *p) { return u32(p)|(uint64_t)u32(p+4)<<32; }

static time_t dos_time(unsigned date,unsigned t)
{
    struct tm tm={0}; tm.tm_isdst=-1;
    tm.tm_year=(int)((date>>9)&0x7f)+80; tm.tm_mon=(int)((date>>5)&0xf)-1; tm.tm_mday=(int)(date&0x1f);
    tm.tm_hour=(int)(t>>11); tm.tm_min=(int)((t>>5)&0x3f); tm.tm_sec=(int)((t&0x1f)*2);
    if(tm.tm_mon<0 || !tm.tm_mday) return 0;
    return mktime(&tm);
}

static int read_at(int fd,void *buf,size_t n,off_t at)
{
    size_t done=0;
    while(done<n) { ssize_t got=pread(fd,(char *)buf+done,n-done,at+(off_t)done); if(got<0 && errno==EINTR) continue; if(got<=0) return -1; done+=(size_t)got; }
    return 0;
}

static int list_zip(Archive *a,char *err,size_t errn)
{
    int fd=open(a->path,O_RDONLY); struct stat st;
    if(fd<0 || fstat(fd,&st)<0) { if(fd>=0) close(fd); snprintf(err,errn,"%s",T("アーカイブを開けません")); return -1; }
    unsigned char *tail=NULL,*cd=NULL; int result=-1;
    size_t tail_n=st.st_size<65557 ? (size_t)st.st_size : 65557;
    if(tail_n<22 || !(tail=malloc(tail_n)) || read_at(fd,tail,tail_n,st.st_size-(off_t)tail_n)<0) goto bad;
    long eocd=-1;
    for(long i=(long)tail_n-22;i>=0;--i) if(u32(tail+i)==0x06054b50u) { eocd=i; break; }
    if(eocd<0) goto bad;
    uint64_t entries=u16(tail+eocd+10),cd_size=u32(tail+eocd+12),cd_off=u32(tail+eocd+16);
    if((entries==0xffff || cd_size==0xffffffffu || cd_off==0xffffffffu) && eocd>=20 && u32(tail+eocd-20)==0x07064b50u) {
        unsigned char z[56]; uint64_t at=u64(tail+eocd-20+8);
        if(at>(uint64_t)st.st_size-56 || read_at(fd,z,56,(off_t)at)<0 || u32(z)!=0x06064b50u) goto bad;
        entries=u64(z+32); cd_size=u64(z+40); cd_off=u64(z+48);
    }
    if(cd_off>(uint64_t)st.st_size || cd_size>(uint64_t)st.st_size-cd_off || cd_size>RUN_OUTPUT_CAP) goto bad;
    if(!(cd=malloc(cd_size ? (size_t)cd_size : 1)) || (cd_size && read_at(fd,cd,(size_t)cd_size,(off_t)cd_off)<0)) goto bad;
    size_t pos=0;
    for(uint64_t i=0;i<entries;++i) {
        if(pos+46>cd_size || u32(cd+pos)!=0x02014b50u) goto bad;
        const unsigned char *e=cd+pos; unsigned n=u16(e+28),x=u16(e+30),c=u16(e+32);
        if(pos+46+n+x+c>cd_size) goto bad;
        char *name=malloc(n+1); if(!name) goto memory;
        memcpy(name,e+46,n); name[n]=0;
        if(memchr(name,0,n)) { free(name); goto bad; }
        Member *m=add_member(a,name,1);
        int dir=n && (name[n-1]=='/' || name[n-1]=='\\');
        free(name);
        if(!m) goto memory;
        if(m==&a->members[a->count]) { pos+=46+n+x+c; continue; }
        uint64_t size=u32(e+24); uint32_t attr=u32(e+38);
        if(size==0xffffffffu) {
            for(const unsigned char *p=e+46+n,*end=p+x;p+4<=end;) {
                unsigned id=u16(p),len=u16(p+2);
                if(p+4+len>end) break;
                if(id==1 && len>=8) { size=u64(p+4); break; }
                p+=4+len;
            }
        }
        unsigned long mode=(e[5]==3) ? attr>>16 : 0;
        m->size=size; m->mtime=dos_time(u16(e+14),u16(e+12));
        m->is_dir=dir || S_ISDIR(mode) || (attr&0x10); m->is_link=S_ISLNK(mode) && !m->is_dir;
        m->mode=mode ? (mode&0777) : m->is_dir ? 0755 : 0644;
        if(m->is_link && u16(e+10)==0 && size<4096) {
            unsigned char lh[30]; uint64_t at=u32(e+42);
            if(at+30<=(uint64_t)st.st_size && !read_at(fd,lh,30,(off_t)at) && u32(lh)==0x04034b50u) {
                uint64_t data=at+30+u16(lh+26)+u16(lh+28);
                char *target=malloc((size_t)size+1);
                if(target && data+size<=(uint64_t)st.st_size && !read_at(fd,target,(size_t)size,(off_t)data)) { target[size]=0; m->link=target; }
                else free(target);
            }
        }
        pos+=46+n+x+c;
    }
    result=0; goto done;
memory: snprintf(err,errn,"%s",T("メモリが足りません")); goto done;
bad: snprintf(err,errn,"%s",T("アーカイブを読めません（zip が壊れています）"));
done: free(tail); free(cd); close(fd); return result;
}

/* ---- LHa (levels 0, 1 and 2; lhasa extracts) ---- */

static int list_lha(Archive *a,char *err,size_t errn)
{
    int fd=open(a->path,O_RDONLY); struct stat st;
    if(fd<0 || fstat(fd,&st)<0) { if(fd>=0) close(fd); snprintf(err,errn,"%s",T("アーカイブを開けません")); return -1; }
    uint64_t off=0,size=(uint64_t)st.st_size; int result=-1;
    unsigned char *h=malloc(65536+4); if(!h) { close(fd); snprintf(err,errn,"%s",T("メモリが足りません")); return -1; }
    for(;;) {
        if(off>=size) break;
        unsigned char first; if(read_at(fd,&first,1,(off_t)off)<0) goto bad;
        if(!first) break;
        if(off+22>size || read_at(fd,h,22,(off_t)off)<0) goto bad;
        if(h[2]!='-' || h[3]!='l' || h[6]!='-') goto bad;
        int level=h[20],is_dir=!memcmp(h+2,"-lhd-",5); size_t hsize; uint64_t packed=u32(h+7),orig=u32(h+11),data;
        char name[1024]=""; char dir[1024]=""; unsigned long mode=0; time_t mtime=0; int have_mode=0;
        if(level==0 || level==1) {
            hsize=(size_t)h[0]+2;
            if(hsize<24 || off+hsize>size || read_at(fd,h,hsize,(off_t)off)<0) goto bad;
            size_t n=h[21]; if(22+n>hsize) goto bad;
            memcpy(name,h+22,n); name[n]=0;
            mtime=dos_time(u16(h+17),u16(h+15));
            uint64_t pos=off+hsize,ext_total=0;
            if(level==1) {
                unsigned next=u16(h+hsize-2);
                while(next) {
                    if(next<3 || pos+next>size || read_at(fd,h,next,(off_t)pos)<0) goto bad;
                    unsigned type=h[0]; size_t len=next-3;
                    if(type==1 && len<sizeof(name)) { memcpy(name,h+1,len); name[len]=0; }
                    if(type==2 && len<sizeof(dir)) { memcpy(dir,h+1,len); dir[len]=0; }
                    if(type==0x50 && len>=2) { mode=u16(h+1); have_mode=1; }
                    if(type==0x54 && len>=4) mtime=(time_t)u32(h+1);
                    pos+=next; ext_total+=next; next=u16(h+next-2);
                }
                if(ext_total>packed) goto bad;
                packed-=ext_total;
            }
            data=pos;
        } else if(level==2) {
            hsize=u16(h);
            if(hsize<26 || off+hsize>size || read_at(fd,h,hsize,(off_t)off)<0) goto bad;
            mtime=(time_t)u32(h+15);
            size_t pos=24; unsigned next=u16(h+pos); pos+=2;
            while(next) {
                if(next<3 || pos+next>hsize) goto bad;
                unsigned type=h[pos]; size_t len=next-3;
                if(type==1 && len<sizeof(name)) { memcpy(name,h+pos+1,len); name[len]=0; }
                if(type==2 && len<sizeof(dir)) { memcpy(dir,h+pos+1,len); dir[len]=0; }
                if(type==0x50 && len>=2) { mode=u16(h+pos+1); have_mode=1; }
                if(type==0x54 && len>=4) mtime=(time_t)u32(h+pos+1);
                unsigned following=u16(h+pos+next-2); pos+=next; next=following;
            }
            data=off+hsize;
        } else { snprintf(err,errn,T("この LHa の形式（レベル %d）は読めません"),level); goto done; }
        char full[2048];
        for(char *p=dir;*p;++p) if((unsigned char)*p==0xff) *p='/';
        for(char *p=name;*p;++p) if((unsigned char)*p==0xff) *p='/';
        snprintf(full,sizeof(full),"%s%s%s",dir,*dir && dir[strlen(dir)-1]!='/' && *name ? "/" : "",name);
        /* LHa for UNIX (and FDTP's p) stores a link as -lhd- "path|target". */
        char *bar=have_mode && S_ISLNK(mode) ? strchr(full,'|') : NULL;
        if(bar) *bar++=0;
        Member *m=add_member(a,full,1);
        if(!m) { snprintf(err,errn,"%s",T("メモリが足りません")); goto done; }
        if(m!=&a->members[a->count]) {
            m->is_dir=is_dir || (*full && full[strlen(full)-1]=='/'); m->size=orig; m->mtime=mtime;
            m->mode=have_mode ? (mode&0777) : m->is_dir ? 0755 : 0644;
            m->is_link=have_mode && S_ISLNK(mode);
            if(m->is_link) {
                m->is_dir=0;
                if(bar && !(m->link=display_utf8(bar))) { snprintf(err,errn,"%s",T("メモリが足りません")); goto done; }
            }
        }
        if(data+packed>size) goto bad;
        off=data+packed;
    }
    result=0; goto done;
bad: snprintf(err,errn,"%s",T("アーカイブを読めません（LHa が壊れています）"));
done: free(h); close(fd); return result;
}

/* Read the listing. err gets a row-23 message on failure. */
static int archive_read(Archive *a,const char *path,char *err,size_t errn)
{
    memset(a,0,sizeof(*a));
    a->format=archive_format(path);
    if(a->format==FMT_NONE) { snprintf(err,errn,"%s",T("アーカイブではありません")); return -1; }
    if(a->format==FMT_7Z) { snprintf(err,errn,"%s",T("この環境では使えません")); return -1; }
    if(a->format==FMT_LHA) { char *l=find_command("lhasa"); if(!l) { snprintf(err,errn,"%s",T("lhasa がありません")); return -1; } free(l); }
    if(!(a->path=realpath(path,NULL))) { snprintf(err,errn,T("アーカイブを開けません: %.100s"),strerror(errno)); return -1; }
    int r=a->format==FMT_TAR ? list_tar(a,err,errn) : a->format==FMT_ZIP ? list_zip(a,err,errn) : list_lha(a,err,errn);
    if(!r) {
        for(size_t i=0;i<a->count;++i) a->link_count+=a->members[i].is_link;
        if(a->link_count && !(a->links=malloc(a->link_count*sizeof(*a->links)))) { snprintf(err,errn,"%s",T("メモリが足りません")); r=-1; }
        else for(size_t i=0,n=0;i<a->count;++i) if(a->members[i].is_link) a->links[n++]=i;
    }
    if(r<0) archive_free(a);
    return r;
}

/* A member would land outside the destination: its own name, a hard link
 * to such a name, or a path below a symbolic link stored earlier in the
 * archive (writing through the link). Returns the offending name or NULL. */
static const char *member_danger(const Archive *a,const Member *m)
{
    if(m->unsafe) return m->raw;
    if(m->hard && m->link) { int unsafe=0; char *p=normalize_name(m->link,0,&unsafe); free(p); if(unsafe) return m->raw; }
    for(size_t i=0;i<a->link_count;++i) {
        const Member *l=&a->members[a->links[i]];
        if(l==m) continue;
        size_t n=strlen(l->path);
        if(!strncmp(m->path,l->path,n) && m->path[n]=='/') return m->raw;
    }
    return NULL;
}
