/*
** require.c - require
**
** See Copyright Notice in mruby.h
*/

#include "mruby.h"
#include "mruby/data.h"
#include "mruby/string.h"
#include "mruby/dump.h"
#include "mruby/proc.h"
#include "mruby/compile.h"
#include "mruby/variable.h"
#include "mruby/array.h"
#include "mruby/numeric.h"

#if defined(_WIN32) || defined(_WIN64) || defined(WIN32) || defined(WIN64)
  #define OS_WINDOWS
#endif

// workaround for new mruby version
#if MRUBY_RELEASE_MAJOR <= 1 && MRUBY_RELEASE_MINOR <= 3
  #define TARGET_CLASS(PROC) PROC->target_class
#else
  #define TARGET_CLASS(PROC) PROC->e.target_class
#endif

#include "opcode.h"
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include <sys/types.h>
#include <limits.h>
#include <setjmp.h>

#if defined(_MSC_VER) || defined(__MINGW32__)
  #ifndef PATH_MAX
    #define PATH_MAX MAX_PATH
  #endif
  #define strdup(x) _strdup(x)
#else
  #include <sys/param.h>
  #include <unistd.h>
  #include <libgen.h>
  #include <dlfcn.h>
#endif

#ifndef MAXPATHLEN
  #define MAXPATHLEN 1024
#endif

#ifndef MAXENVLEN
  #define MAXENVLEN 1024
#endif

#ifdef OS_WINDOWS

  #include <windows.h>

  static
  int
  relativeToFullPath( char const path[], char full_path[MAXPATHLEN] ) {
    DWORD retval = GetFullPathNameA( path, MAXPATHLEN, full_path, NULL );
    return retval > 0 && retval < MAXPATHLEN ;
  }

  static
  int
  GetEnvironmentToString( char const envName[], char out[], unsigned len ) {
    DWORD n = GetEnvironmentVariableA(envName,out,(DWORD)len);
    return n > 0 && n < (DWORD)len ;
  }

  static
  void
  CheckError( char const lib[], mrb_state *mrb ) {
    // Get the error message, if any.
    DWORD errorMessageID = GetLastError();
    if ( errorMessageID == 0 ) return ; // No error message has been recorded
    printf("errorMessageID: %ld\n", errorMessageID);

    LPSTR messageBuffer = NULL;
    size_t size = FormatMessageA( FORMAT_MESSAGE_ALLOCATE_BUFFER |
                                  FORMAT_MESSAGE_FROM_SYSTEM |
                                  FORMAT_MESSAGE_IGNORE_INSERTS,
                                  NULL,
                                  errorMessageID,
                                  MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                  (LPSTR)&messageBuffer,
                                  0,
                                  NULL );

    printf("failed to load DLL: %s\n", lib);
    mrb_raise( mrb, E_RUNTIME_ERROR, messageBuffer ) ;
    // Free the buffer.
    LocalFree(messageBuffer);
  }

#else

  #include <dlfcn.h>

  static
  int
  relativeToFullPath( char const path[], char full_path[MAXPATHLEN] ) {
    return realpath(path, full_path) != NULL ;
  }

  static
  int
  GetEnvironmentToString( char const envName[], char out[], unsigned len ) {
    char const * ptr = getenv( envName ) ;
    int ok = ptr != NULL && strlen( ptr ) < len ;
    if ( ok ) strcpy( out, ptr ) ;
    return ok ;
  }

  static
  void
  CheckError( char const lib[], mrb_state *mrb ) {
    char const * err = dlerror() ;
    if ( err != NULL ) {
      printf("failed to load DLL: %s\n", lib);
      mrb_raise( mrb, E_RUNTIME_ERROR, dlerror() );
    }
  }

#endif

#ifdef OS_WINDOWS
  #define ENV_SEP ';'
#else
  #define ENV_SEP ':'
#endif

#define E_LOAD_ERROR (mrb_class_get(mrb, "ScriptError"))

static
mrb_value
envpath_to_mrb_ary( mrb_state *mrb, char const name[] ) {

  mrb_value ary = mrb_ary_new(mrb);
  
  char env[MAXENVLEN];
  if ( !GetEnvironmentToString( name, env, MAXENVLEN ) ) return ary;

  unsigned long envlen = strlen(env);
  unsigned long i      = 0 ;
  while ( i < envlen ) {
    char *ptr = env + i;
    char *end = strchr(ptr, ENV_SEP);
    if ( end == NULL ) end = env + envlen;
    unsigned long len = end - ptr;
    mrb_ary_push(mrb, ary, mrb_str_new(mrb, ptr, len));
    i += len+1;
  }

  return ary;
}


static
mrb_value
find_file_check( mrb_state *mrb,
                 mrb_value mrb_path,
                 mrb_value mrb_fname,
                 mrb_value ext ) {

  mrb_value mrb_filepath = mrb_str_dup(mrb, mrb_path);
  mrb_str_cat2(mrb, mrb_filepath, "/");
  mrb_str_buf_append(mrb, mrb_filepath, mrb_fname);

  if ( !mrb_nil_p(ext) ) mrb_str_buf_append( mrb, mrb_filepath, ext );
  if ( mrb_nil_p(mrb_filepath) ) return mrb_nil_value();

  char fpath[MAXPATHLEN];
  if ( ! relativeToFullPath( RSTRING_PTR(mrb_filepath),fpath ) ) return mrb_nil_value();

#ifdef OS_WINDOWS
  FILE * fp ;
  errno_t err = fopen_s(&fp,fpath, "r");
  if ( err != 0 ) return mrb_nil_value();
  fclose(fp);
#else
  FILE * fp = fopen(fpath, "r");
  if ( fp == NULL ) return mrb_nil_value();
  fclose(fp);
#endif
  return mrb_str_new_cstr( mrb, fpath ) ;
}

static
char const *
file_basename( char const fname[] ) {
  char const * tmp ;
  char const * ptr ;

  // search last / or \\ character
  ptr = tmp = fname ;
  while ( tmp ) {
    if ( (tmp = strchr(ptr, '/' )) ||
         (tmp = strchr(ptr, '\\')) ) ptr = tmp + 1;
  }

  return ptr ;
}

static
mrb_value
find_file( mrb_state *mrb, mrb_value mrb_filename ) {

  printf( "require:find_file: %s\n", RSTRING_PTR(mrb_filename)) ;

  mrb_value load_path = mrb_obj_dup(mrb, mrb_gv_get(mrb, mrb_intern_cstr(mrb, "$:")));

  load_path = mrb_check_array_type(mrb, load_path);

  if ( mrb_nil_p(load_path) ) {
    mrb_raise(mrb, E_RUNTIME_ERROR, "invalid $:");
    return mrb_undef_value();
  }

  char const * fname = RSTRING_PTR(mrb_filename);
  char const * ptr   = file_basename(fname) ;
  char const * ext   = strrchr(ptr, '.');
  mrb_value    exts  = mrb_ary_new(mrb);
  if (ext == NULL) {
    mrb_ary_push(mrb, exts, mrb_str_new_cstr(mrb, ".rb"));
    mrb_ary_push(mrb, exts, mrb_str_new_cstr(mrb, ".mrb"));
    mrb_ary_push(mrb, exts, mrb_str_new_cstr(mrb, ".so"));
  } else {
    mrb_ary_push(mrb, exts, mrb_nil_value());
  }

  /* Absolute paths on Windows */
#ifdef OS_WINDOWS
  if (fname[1] == ':') {
    FILE * fp ;
    errno_t err = fopen_s(&fp,fname, "r");
    if ( err != 0 ) goto not_found;
    fclose(fp);
    return mrb_filename;
  }
#endif
  /* when absolute path */
  if (*fname == '/') {
    FILE * fp = fopen(fname, "r");
    if ( fp == NULL ) goto not_found;
    fclose(fp);
    return mrb_filename;
  }

  /* when a filename start with '.', $: = ['.'] */
  if ( *fname == '.' ) {
    load_path = mrb_ary_new(mrb);
    mrb_ary_push(mrb, load_path, mrb_str_new_cstr(mrb, "."));
  }

  for ( int i = 0; i < RARRAY_LEN(load_path); ++i ) {
    for ( int j = 0; j < RARRAY_LEN(exts); ++j ) {
      mrb_value mrb_filepath = find_file_check( mrb,
                                                mrb_ary_entry(load_path, i),
                                                mrb_filename,
                                                mrb_ary_entry(exts, j) );
      if ( !mrb_nil_p(mrb_filepath) ) return mrb_filepath;
    }
  }

not_found:
  mrb_raisef(mrb, E_LOAD_ERROR, "cannot load such file -- %S", mrb_filename);
  return mrb_nil_value();
}

static
void
replace_stop_with_return( mrb_state *mrb, mrb_irep *irep ) {
  if (irep->iseq[irep->ilen - 1] == MKOP_A(OP_STOP, 0)) {
    if (irep->flags == MRB_ISEQ_NO_FREE) {
      mrb_code* iseq = mrb_malloc(mrb, (irep->ilen + 1) * sizeof(mrb_code));
      memcpy(iseq, irep->iseq, irep->ilen * sizeof(mrb_code));
      irep->iseq = iseq;
      irep->flags &= ~MRB_ISEQ_NO_FREE;
    } else {
      irep->iseq = mrb_realloc(mrb, irep->iseq, (irep->ilen + 1) * sizeof(mrb_code));
    }
    irep->iseq[irep->ilen - 1] = MKOP_A(OP_LOADNIL, 0);
    irep->iseq[irep->ilen] = MKOP_AB(OP_RETURN, 0, OP_R_NORMAL);
    irep->ilen++;
  }
}

static
void
load_mrb_file( mrb_state *mrb, mrb_value mrb_filepath ) {

  printf( "require:load_mrb_file\n") ;

  char const * fpath = RSTRING_PTR(mrb_filepath);
  {
    #ifdef OS_WINDOWS
    FILE * fp ;
    errno_t err = fopen_s(&fp,fpath, "rb");
    if ( err != 0 ) {
    #else
    FILE * fp = fopen(fpath, "rb");
    if (fp == NULL) {
    #endif
      mrb_raisef(mrb, E_LOAD_ERROR, "can't load %S", mrb_str_new_cstr(mrb, fpath));
      return;
    }
    fclose(fp);
  }

  int arena_idx = mrb_gc_arena_save(mrb);

  #ifdef OS_WINDOWS
  FILE * fp ;
  errno_t err = fopen_s(&fp,fpath, "rb"); // ignore error
  #else
  FILE * fp = fopen(fpath, "rb");
  #endif

  mrb_irep * irep = mrb_read_irep_file(mrb, fp);
  fclose(fp);

  mrb_gc_arena_restore(mrb, arena_idx);

  if (irep) {
    replace_stop_with_return(mrb, irep);
    struct RProc * proc = mrb_proc_new(mrb, irep);
    TARGET_CLASS(proc) = mrb->object_class; // changed RProc with a union

    arena_idx = mrb_gc_arena_save(mrb);
    mrb_yield_with_class( mrb,
                          mrb_obj_value(proc),
                          0,
                          NULL,
                          mrb_top_self(mrb),
                          mrb->object_class );
    mrb_gc_arena_restore(mrb, arena_idx);
  } else if (mrb->exc) {
    // fail to load
    longjmp(*(jmp_buf*)mrb->jmp, 1);
  }
}

static
void
mrb_load_irep_data( mrb_state* mrb, const uint8_t* data ) {

  printf( "require:mrb_load_irep_data\n") ;

  int ai = mrb_gc_arena_save(mrb);
  mrb_irep *irep = mrb_read_irep(mrb,data);
  mrb_gc_arena_restore(mrb,ai);

  if (irep) {
    replace_stop_with_return(mrb, irep);
    struct RProc *proc = mrb_proc_new(mrb, irep);
    TARGET_CLASS(proc) = mrb->object_class; // changed RProc with a union

    int ai = mrb_gc_arena_save(mrb);
    mrb_yield_with_class( mrb,
                          mrb_obj_value(proc),
                          0,
                          NULL,
                          mrb_top_self(mrb),
                          mrb->object_class );
    mrb_gc_arena_restore(mrb, ai);
  } else if (mrb->exc) {
    // fail to load
    longjmp(*(jmp_buf*)mrb->jmp, 1);
  }
}

static
void
load_so_file( mrb_state *mrb, mrb_value mrb_filepath ) {
  char entry[PATH_MAX]      = {0};
  char entry_irep[PATH_MAX] = {0};
  typedef void (*fn_mrb_gem_init)(mrb_state *mrb);

  char const * filepath = RSTRING_PTR(mrb_filepath);
  printf( "require:load_so_file: %s\n", filepath) ;

  #ifdef OS_WINDOWS
  HMODULE handle = LoadLibrary(filepath);
  #else
  void * handle = dlopen(filepath, RTLD_LAZY|RTLD_GLOBAL);
  #endif

  if ( handle == NULL ) {
    printf( "require:load_so_file: null handle, check error\n" ) ;
    CheckError( filepath, mrb ) ;
    char message[1024] ;
    snprintf( message, 1023, "failed to load %s, open return a NULL pointer\n", filepath );
    printf( "%s", message ) ;
    mrb_raise(mrb, E_LOAD_ERROR, message );
  }

  char * ptr = strdup(file_basename(filepath)) ;
  char * tmp = strrchr(ptr, '.');
  if (tmp) *tmp = 0;
  for ( tmp = ptr ; *tmp ; ++tmp ) { if (*tmp == '-') *tmp = '_' ; }

  snprintf(entry,      sizeof(entry)-1,      "mrb_%s_gem_init",    ptr);
  snprintf(entry_irep, sizeof(entry_irep)-1, "gem_mrblib_irep_%s", ptr);
  free(ptr);

  printf( "require:load_so_file attach entry\n") ;

  #ifdef OS_WINDOWS
  FARPROC addr_entry      = GetProcAddress(handle, entry);
  FARPROC addr_entry_irep = GetProcAddress(handle, entry_irep);
  #else
  void * addr_entry      = dlsym(handle, entry);
  void * addr_entry_irep = dlsym(handle, entry_irep);
  #endif

  if ( addr_entry == NULL && addr_entry_irep == NULL ) {
    char message[1024] ;
    snprintf( message, 1023, "failed to attach %s or %s in library %s\n",
              entry, entry_irep, filepath );
    printf( "%s", message ) ;
    mrb_raise(mrb, E_LOAD_ERROR, message );
  }

  if ( addr_entry != NULL ) {
    printf( "Attach %s from library %s\n", entry, mrb_filepath );
    fn_mrb_gem_init fn = (fn_mrb_gem_init) addr_entry;
    int ai = mrb_gc_arena_save(mrb);
    fn(mrb);
    mrb_gc_arena_restore(mrb, ai);
  }

  if ( addr_entry_irep != NULL ) {
    printf( "Attach %s from library %s\n", entry_irep, mrb_filepath );
    uint8_t const * data = (uint8_t const *) addr_entry_irep;
    mrb_load_irep_data(mrb, data);
  }

}

static
void
unload_so_file(mrb_state *mrb, mrb_value mrb_filepath) {


  char entry[PATH_MAX] = {0} ;
  typedef void (*fn_mrb_gem_final)(mrb_state *mrb);

  char const * filepath = RSTRING_PTR(mrb_filepath);

  printf( "require:unload_so_file: %s\n", filepath) ;

  #ifdef OS_WINDOWS
  HMODULE handle = LoadLibrary(filepath);
  #else
  void * handle = dlopen(filepath, RTLD_LAZY|RTLD_GLOBAL);
  #endif

  if ( handle == NULL ) {
    CheckError( filepath, mrb ) ;
    char message[1024] ;
    snprintf( message, 1023, "failed to load %s, open return a NULL pointer\n", filepath );
    printf( "%s", message ) ;
    mrb_raise(mrb, E_LOAD_ERROR, message );
  }

  char * ptr = strdup(file_basename(filepath)) ;
  char * tmp = strrchr(ptr, '.');
  if (tmp) *tmp = 0;
  for ( tmp = ptr ; *tmp ; ++tmp ) { if (*tmp == '-') *tmp = '_'; }
  snprintf(entry, sizeof(entry)-1, "mrb_%s_gem_final", ptr);
  free(tmp);

  #ifdef OS_WINDOWS
  FARPROC addr_entry = GetProcAddress(handle, entry);
  #else
  void * addr_entry  = dlsym(handle, entry);
  #endif

  if ( addr_entry == NULL ) {
    mrb_raisef(mrb, E_LOAD_ERROR, "can't attach %S", entry);
  } else {
    fn_mrb_gem_final fn = (fn_mrb_gem_final) addr_entry ;
    fn(mrb);
  }
}


static void
mrb_load_fail(mrb_state *mrb, mrb_value path, const char *err)
{
  mrb_value mesg, exc;

  mesg = mrb_str_new_cstr(mrb, err);
  mrb_str_cat_lit(mrb, mesg, " -- ");
  mrb_str_cat_str(mrb, mesg, path);
  exc = mrb_funcall(mrb, mrb_obj_value(E_LOAD_ERROR), "new", 1, mesg);
  mrb_iv_set(mrb, exc, mrb_intern_lit(mrb, "path"), path);

  mrb_exc_raise(mrb, exc);
}

static
void
load_rb_file( mrb_state *mrb, mrb_value mrb_filepath ) {

  printf( "require:load_rb_file: %s\n", RSTRING_PTR(mrb_filepath)) ;

  //int ai = mrb_gc_arena_save(mrb);

  char const * filepath = RSTRING_PTR(mrb_filepath);
  {
    #ifdef OS_WINDOWS
    FILE * fp ;
    errno_t err = fopen_s(&fp,filepath, "r");
    if ( err != 0 ) {
    #else
    FILE *fp = fopen(filepath, "r");
    if (fp == NULL) {
    #endif
      mrb_load_fail( mrb, mrb_filepath, "cannot load such file" );
      return;
    }
    fclose(fp);
  }

  #ifdef OS_WINDOWS
  FILE * file ;
  errno_t err = fopen_s(&file,filepath, "r");
  #else
  FILE * file = fopen(filepath, "r");
  #endif

  mrbc_context * mrbc_ctx = mrbc_context_new(mrb);

  mrbc_filename( mrb, mrbc_ctx, filepath );
  mrb_gv_set(mrb, mrb_intern_cstr(mrb, "$0"), mrb_filepath);

  printf( "call mrb_load_file_cxt\n") ;
  mrb_value obj = mrb_load_file_cxt( mrb, file, mrbc_ctx );
  printf( "done mrb_load_file_cxt\n") ;

  mrbc_context_free(mrb, mrbc_ctx );

  fclose( file );

  if (mrb->exc) mrb_print_error(mrb);

  //mrb_gc_arena_restore(mrb, ai);

}

static
void
load_file( mrb_state *mrb, mrb_value mrb_filepath ) {
  char const * ext = strrchr(RSTRING_PTR(mrb_filepath), '.');

  printf( "require:load_file: %s\n", RSTRING_PTR(mrb_filepath)) ;

  if (!ext || strcmp(ext, ".rb") == 0) {
    load_rb_file(mrb, mrb_filepath);
  } else if (strcmp(ext, ".mrb") == 0) {
    load_mrb_file(mrb, mrb_filepath);
  } else if (strcmp(ext, ".so") == 0 || 
             strcmp(ext, ".dll") == 0 || 
             strcmp(ext, ".dylib") == 0) {
    load_so_file(mrb, mrb_filepath);
  } else {
    mrb_raisef(mrb, E_LOAD_ERROR, "Filepath '%S' has invalid extension.", mrb_filepath);
    return;
  }
}

mrb_value
mrb_load( mrb_state *mrb, mrb_value filename ) {

  printf("mrb_load\n");

  mrb_value mrb_filepath = find_file(mrb, filename);
  load_file(mrb, mrb_filepath);
  return mrb_true_value(); // TODO: ??
}

mrb_value
mrb_f_load( mrb_state *mrb, mrb_value self ) {

  printf("mrb_f_load\n");

  mrb_value mrb_filename;
  mrb_get_args(mrb, "o", &mrb_filename);
  if (mrb_type(mrb_filename) != MRB_TT_STRING) {
    mrb_raisef(mrb, E_TYPE_ERROR, "can't convert %S into String", mrb_filename);
    return mrb_nil_value();
  }
  return mrb_load(mrb, mrb_filename);
}

static
int
loaded_files_check( mrb_state *mrb, mrb_value mrb_filepath ) {

  printf("loaded_files_check\n");

  mrb_value loaded_files = mrb_gv_get(mrb, mrb_intern_cstr(mrb, "$\""));

  for ( int i = 0; i < RARRAY_LEN(loaded_files); ++i ) {
    if ( mrb_str_cmp( mrb,
                      mrb_ary_entry(loaded_files, i),
                      mrb_filepath ) == 0 ) {
      return 0;
    }
  }

  mrb_value loading_files = mrb_gv_get(mrb, mrb_intern_cstr(mrb, "$\"_"));
  if ( mrb_nil_p(loading_files) ) return 1;
  for ( int i = 0; i < RARRAY_LEN(loading_files); ++i ) {
    if ( mrb_str_cmp( mrb,
                      mrb_ary_entry(loading_files, i),
                      mrb_filepath ) == 0 ) {
      return 0;
    }
  }

  return 1;
}

static
void
loading_files_add( mrb_state *mrb, mrb_value mrb_filepath ) {
  mrb_value loading_files = mrb_gv_get(mrb, mrb_intern_cstr(mrb, "$\"_"));
  if ( mrb_nil_p(loading_files) ) loading_files = mrb_ary_new(mrb);
  mrb_ary_push(mrb, loading_files, mrb_filepath);
  mrb_gv_set(mrb, mrb_intern_cstr(mrb, "$\"_"), loading_files);
}

static
void
loaded_files_add( mrb_state *mrb, mrb_value mrb_filepath ) {
  mrb_value loaded_files = mrb_gv_get(mrb, mrb_intern_cstr(mrb, "$\""));
  mrb_ary_push(mrb, loaded_files, mrb_filepath);
  mrb_gv_set(mrb, mrb_intern_cstr(mrb, "$\""), loaded_files);
}

mrb_value
mrb_require( mrb_state *mrb, mrb_value mrb_filename ) {

  printf("mrb_require\n");

  mrb_value mrb_filepath = find_file(mrb, mrb_filename);
  if ( !mrb_nil_p(mrb_filepath) && loaded_files_check(mrb, mrb_filepath) ) {
    loading_files_add(mrb, mrb_filepath);
    load_file(mrb, mrb_filepath);
    loaded_files_add(mrb, mrb_filepath);
    return mrb_true_value();
  }
  return mrb_false_value();
}

mrb_value
mrb_f_require( mrb_state *mrb, mrb_value self ) {

  printf("mrb_f_require\n");

  mrb_value mrb_filename;
  mrb_get_args(mrb, "o", &mrb_filename);
  if (mrb_type(mrb_filename) != MRB_TT_STRING) {
    mrb_raisef(mrb, E_TYPE_ERROR, "can't convert %S into String", mrb_filename);
    return mrb_nil_value();
  }
  return mrb_require(mrb, mrb_filename);
}

static
mrb_value
mrb_init_load_path( mrb_state *mrb ) {

  printf("mrb_init_load_path\n");

  mrb_value ary = envpath_to_mrb_ary(mrb, "MRBLIB");
  char env[MAXENVLEN];
  if ( GetEnvironmentToString( "MRBGEMS_ROOT", env, MAXENVLEN ) )
    mrb_ary_push(mrb, ary, mrb_str_new_cstr(mrb, env));
#ifdef MRBGEMS_ROOT
  else
    mrb_ary_push(mrb, ary, mrb_str_new_cstr(mrb, MRBGEMS_ROOT));
#endif

  return ary;
}

void
mrb_mruby_require_gem_init( mrb_state* mrb ) {
  struct RClass *krn = mrb->kernel_module;

  mrb_define_method(mrb, krn, "load",    mrb_f_load,    MRB_ARGS_REQ(1));
  mrb_define_method(mrb, krn, "require", mrb_f_require, MRB_ARGS_REQ(1));

  mrb_gv_set(mrb, mrb_intern_cstr(mrb, "$:"), mrb_init_load_path(mrb));
  mrb_gv_set(mrb, mrb_intern_cstr(mrb, "$\""), mrb_ary_new(mrb));

  char env[MAXENVLEN];
  if ( GetEnvironmentToString( "MRUBY_REQUIRE", env, MAXENVLEN ) ) {
    size_t envlen = strlen(env);
    size_t i      = 0 ;
    while ( i < envlen ) {
      char *ptr = env + i;
      char *end = strchr(ptr, ',');
      if (end == NULL) end = env + envlen;
      size_t len = end - ptr;
      mrb_require(mrb, mrb_str_new(mrb, ptr, len));
      i += len+1;
    }
  }
}

void
mrb_mruby_require_gem_final( mrb_state* mrb ) {
  mrb_value loaded_files = mrb_gv_get(mrb, mrb_intern_cstr(mrb, "$\""));
  for ( int i = 0; i < RARRAY_LEN(loaded_files); ++i ) {
    mrb_value f = mrb_ary_entry(loaded_files, i);
    const char * ext = strrchr(RSTRING_PTR(f), '.');
    if (ext && strcmp(ext, ".so") == 0) unload_so_file(mrb, f);
  }
}

/* vim:set et ts=2 sts=2 sw=2 tw=0: */
