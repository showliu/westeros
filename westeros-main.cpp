#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include <memory.h>
#include <assert.h>
#include <dirent.h>
#include <poll.h>
#include <pthread.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <linux/input.h>

#include <vector>

#include "westeros-compositor.h"

static bool g_running= false;

static void signalHandler(int signum)
{
   printf("signalHandler: signum %d\n", signum);
	g_running= false;
}

static void showUsage()
{
   printf("usage:\n");
   printf(" westeros [options]\n" );
   printf("where [options] are:\n" );
   printf("  --renderer <module> : renderer module to use\n" );
   printf("  --framerate <rate> : frame rate in fps\n" );
   printf("  --display <name> : name of wayland display created by compositor\n" );
   printf("  --nested : operate as a nested compositor\n" );
   printf("  --nestedDisplay <name> : name of wayland display to connect to for nested composition\n" );
   printf("  --width <width> : width of nested composition surface\n" );
   printf("  --height <width> : height of nested composition surface\n" );
   printf("  -? : show usage\n" );
   printf("\n" );
}

static const char *inputByPath= "/dev/input/by-path/";
static const char *kbdDev= "event-kbd";
static const char *mouseDev= "event-mouse";

int openDevice( std::vector<pollfd> &deviceFds, const char *devPathName )
{
   int fd= open( devPathName, O_RDONLY | O_CLOEXEC );
   if ( fd < 0 )
   {
      printf( "error opening device: %s\n", devPathName );
   }
   else
   {
      pollfd pfd;
      printf( "opened device %s : fd %d\n", devPathName, fd );
      pfd.fd= fd;
      deviceFds.push_back( pfd );
   }
}

char *getDevice( const char *devType, const char *path, char *devName )
{
   int len, lenDev;
   char *devPathName= 0;
   
   len= strlen( devName );
   
   lenDev= strlen(devType);
   if ( len > lenDev )
   {
     if ( !strncmp( devName+len-lenDev, devType, lenDev) )
     {
        devPathName= (char *)malloc( strlen(path)+len+1);
        if ( devPathName )
        {
           strcpy( devPathName, path );
           strcat( devPathName, devName );
           
           printf( "found %s: %s\n", devType, devPathName );           
        }
     }
   }
   
   return devPathName;
}

void getDevices( std::vector<pollfd> &deviceFds )
{
   int maxName, buffSize;
   DIR * dir;
   struct dirent *entry= 0;
   struct dirent *result;
   char *devPathName;
   
   maxName= pathconf( inputByPath, _PC_NAME_MAX );
   if ( maxName < 0 ) maxName= 255;
   
   buffSize= offsetof(struct dirent, d_name) + maxName + 1;
   entry= (struct dirent*)malloc( buffSize );
   if ( entry )
   {
      dir= opendir( inputByPath );
      if ( dir )
      {
         while( !readdir_r( dir, entry, &result) )
         {
            if ( !result )
            {
               break;
            }
            
            devPathName= getDevice( kbdDev, inputByPath, result->d_name );
            if ( !devPathName )
            {
               devPathName= getDevice( mouseDev, inputByPath, result->d_name );
            }

            if ( devPathName )
            {
               openDevice( deviceFds, devPathName );
               free( devPathName );
            }
         }
         
         closedir( dir );
      }
      free( entry );
   }
}

void releaseDevices( std::vector<pollfd> &deviceFds )
{
   while( deviceFds.size() > 0 )
   {
      pollfd pfd= deviceFds[0];
      printf( "closing device fd: %d\n", pfd.fd );
      close( pfd.fd );
      deviceFds.erase( deviceFds.begin() );
   }
}

typedef struct _inputContext
{
   bool started;
   bool stopRequested;
   std::vector<pollfd> deviceFds;
   WstCompositor *wctx;
} inputContext;

void* inputThread( void *data )
{
   inputContext *inCtx= (inputContext*)data;
   int deviceCount= inCtx->deviceFds.size();
   int i, n;
   input_event e;
   unsigned int keyModifiers= 0;
   int mouseAccel= 1;
   int mouseX= 0;
   int mouseY= 0;
   unsigned int outputWidth, outputHeight;
   bool mouseEnterSent= false;
   bool mouseMoved= false;
   
   inCtx->started= true;
      
   while( !inCtx->stopRequested )
   {
      for( i= 0; i < deviceCount; ++i )
      {
         inCtx->deviceFds[i].events= POLLIN | POLLERR;
         inCtx->deviceFds[i].revents= 0;
      }
      
      n= poll(&inCtx->deviceFds[0], deviceCount, 400);
      if ( n >= 0 )
      {
         for( i= 0; i < deviceCount; ++i )
         {
            if ( inCtx->deviceFds[i].revents & POLLIN )
            {
               n= read( inCtx->deviceFds[i].fd, &e, sizeof(input_event) );
               if ( n > 0 )
               {
                  switch( e.type )
                  {
                     case EV_KEY:
                        switch( e.code )
                        {
                           case BTN_LEFT:
                           case BTN_RIGHT:
                           case BTN_MIDDLE:
                           case BTN_SIDE:
                           case BTN_EXTRA:
                              {
                                 unsigned int keyCode= e.code;
                                 unsigned int keyState;
                                 
                                 if ( !mouseEnterSent )
                                 {
                                    WstCompositorPointerEnter( inCtx->wctx );
                                    mouseEnterSent= true;
                                 }
                                 switch ( e.value )
                                 {
                                    case 0:
                                       keyState= WstKeyboard_keyState_released;
                                       break;
                                    case 1:
                                       keyState= WstKeyboard_keyState_depressed;
                                       break;
                                    default:
                                       keyState= WstKeyboard_keyState_none;
                                       break;
                                 }

                                 if ( keyState != WstKeyboard_keyState_none )
                                 {
                                    WstCompositorPointerButtonEvent( inCtx->wctx, keyCode, keyState );
                                 }
                              }
                              break;
                           default:
                              {
                                 int keyCode= e.code;
                                 unsigned int keyState;

                                 switch( keyCode )
                                 {
                                    case KEY_LEFTSHIFT:
                                    case KEY_RIGHTSHIFT:
                                       if ( e.value )
                                          keyModifiers |= WstKeyboard_shift;
                                       else
                                          keyModifiers &= ~WstKeyboard_shift;
                                       break;
                                       
                                    case KEY_LEFTCTRL:
                                    case KEY_RIGHTCTRL:
                                       if ( e.value )
                                          keyModifiers |= WstKeyboard_ctrl;
                                       else
                                          keyModifiers &= ~WstKeyboard_ctrl;
                                       break;

                                    case KEY_LEFTALT:
                                    case KEY_RIGHTALT:
                                       if ( e.value )
                                          keyModifiers |= WstKeyboard_alt;
                                       else
                                          keyModifiers &= ~WstKeyboard_alt;
                                       break;
                                     default:
                                        {
                                          switch ( e.value )
                                          {
                                             case 0:
                                                keyState= WstKeyboard_keyState_released;
                                                break;
                                             case 1:
                                                keyState= WstKeyboard_keyState_depressed;
                                                break;
                                             default:
                                                keyState= WstKeyboard_keyState_none;
                                                break;
                                          }

                                          if ( keyState != WstKeyboard_keyState_none )
                                          {
                                             WstCompositorKeyEvent( inCtx->wctx,
                                                                    keyCode,
                                                                    keyState,
                                                                    keyModifiers );
                                          }                                          
                                        }
                                        break;
                                 }                                 
                              }
                              break;
                        }
                        break;
                     case EV_REL:
                        if ( !outputWidth || !outputHeight )
                        {
                           WstCompositorGetOutputDimensions( inCtx->wctx, &outputWidth, &outputHeight );
                        }
                        switch( e.code )
                        {
                           case REL_X:
                              mouseX= mouseX + e.value * mouseAccel;
                              if ( mouseX < 0 ) mouseX= 0;
                              if ( mouseX > outputWidth ) mouseX= outputWidth;
                              mouseMoved= true;
                              break;
                           case REL_Y:
                              mouseY= mouseY + e.value * mouseAccel;
                              if ( mouseY < 0 ) mouseY= 0;
                              if ( mouseY > outputHeight ) mouseY= outputHeight;
                              mouseMoved= true;
                              break;
                           default:
                              break;
                        }
                        break;
                     case EV_SYN:
                        {
                           if ( mouseMoved )
                           {
                              if ( !mouseEnterSent )
                              {
                                 WstCompositorPointerEnter( inCtx->wctx );
                                 mouseEnterSent= true;
                              }
                           
                              WstCompositorPointerMoveEvent( inCtx->wctx, mouseX, mouseY );
                              
                              mouseMoved= false;
                           }
                        }
                        break;
                     default:
                        break;
                  }
               }
            }
         }
      }
   }
   
   return NULL;
}

int main( int argc, char** argv)
{
   int nRC= 0;
	struct sigaction sigint;
   const char *rendererModule= 0;
   const char *displayName= 0;
   const char *nestedDisplayName= 0;
   bool error= false;
   int len, value, width=-1, height=-1;
   pthread_t inputThreadId;
   inputContext inputCtx;
   WstCompositor *wctx;

   inputCtx.started= false;
   inputCtx.stopRequested= false;

   wctx= WstCompositorCreate();
   if ( !wctx )
   {
      printf("unable to create compositor instance\n");
      nRC= -1;
      goto exit;
   }

   for( int i= 1; i < argc; ++i )
   {
      len= strlen(argv[i]);
      if ( (len == 10) && !strncmp( (const char*)argv[i], "--renderer", len) )
      {
         if ( i < argc-1 )
         {
            ++i;
            rendererModule= argv[i];
            
            if ( !WstCompositorSetRendererModule( wctx, rendererModule) )
            {
               error= true;
               break;
            }
         }
      }
      else
      if ( (len == 11) && !strncmp( (const char*)argv[i], "--framerate", len) )
      {
         if ( i < argc-1 )
         {
            int frameRate;
            
            ++i;
            frameRate= atoi(argv[i]);
            if ( frameRate > 0 )
            {
               if ( !WstCompositorSetFrameRate( wctx, frameRate ) )
               {
                  error= true;
                  break;
               }
            }
         }
      }
      else
      if ( (len == 9) && !strncmp( (const char*)argv[i], "--display", len) )
      {
         if ( i < argc-1)
         {
            ++i;
            displayName= argv[i];

            if ( !WstCompositorSetDisplayName( wctx, displayName) )
            {
               error= true;
               break;
            }
         }
      }
      if ( (len == 8) && !strncmp( (const char*)argv[i], "--nested", len) )
      {
         if ( !WstCompositorSetIsNested( wctx, true) )
         {
            error= true;
            break;
         }
      }
      else
      if ( (len == 15) && !strncmp( (const char*)argv[i], "--nestedDisplay", len) )
      {
         if ( i < argc-1)
         {
            ++i;
            nestedDisplayName= argv[i];

            if ( !WstCompositorSetNestedDisplayName( wctx, nestedDisplayName) )
            {
               error= true;
               break;
            }
         }
      }
      else
      if ( (len == 7) && !strncmp( argv[i], "--width", len) )
      {
         if ( i+1 < argc )
         {
            ++i;
            value= atoi(argv[i]);
            if ( value > 0 )
            {
               width= value;
            }
         }
      }
      else
      if ( (len == 8) && !strncmp( argv[i], "--height", len) )
      {
         if ( i+1 < argc )
         {
            ++i;
            value= atoi(argv[i]);
            if ( value > 0 )
            {
               height= value;
            }
         }
      }
      else
      if ( (len == 2) && !strncmp( (const char*)argv[i], "-?", len) )
      {
         showUsage();
         goto exit;
      }
   }
   
   if ( !error )
   {
      if ( !WstCompositorSetAllowCursorModification( wctx, true ) )
      {
         error= true;
         goto exit;
      }
      
      if ( (width > 0) && (height > 0) )
      {
         if ( !WstCompositorSetNestedSize( wctx, width, height) )
         {
            error= true;
            goto exit;
         }
      }

      if ( !rendererModule )
      {
         printf("missing renderer module: use --renderer <module>\n");
         nRC= -1;
         goto exit;
      }
      
      if ( !error )
      {
         if ( !WstCompositorGetIsNested( wctx ) )
         {
            getDevices( inputCtx.deviceFds );
            if ( inputCtx.deviceFds.size() > 0 )
            {
               inputCtx.wctx= wctx;
               int rc= pthread_create( &inputThreadId, NULL, inputThread, &inputCtx );
               if ( rc )
               {
                  printf("unable to start input thread: error %d\n", rc );
               }
            }
         }
      
         if ( !(error= !WstCompositorStart( wctx )) )
         {
	         sigint.sa_handler = signalHandler;
	         sigemptyset(&sigint.sa_mask);
	         sigint.sa_flags = SA_RESETHAND;
	         sigaction(SIGINT, &sigint, NULL);

            g_running= true;
            while( g_running )
            {
               usleep( 10000 );
            }
            
            WstCompositorStop( wctx );
         }
      }
   }
      
exit:

   if ( !WstCompositorGetIsNested( wctx ) )
   {
      if ( inputCtx.started )
      {
         inputCtx.stopRequested= true;
         pthread_join( inputThreadId, NULL );
      }
      releaseDevices( inputCtx.deviceFds );
   }
   
   if ( wctx )
   {
      if ( error )
      {
         const char *detail= WstCompositorGetLastErrorDetail( wctx );
         printf("Compositor error: (%s)\n", detail );
      }
      
      WstCompositorDestroy( wctx );
   }

   return nRC;   
}
