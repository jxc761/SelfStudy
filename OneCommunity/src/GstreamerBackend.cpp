/////////////////////////////////////////////////////////////////////////////
// Purpose:     GStreamer backend playing streaming media for wxWidgets 
// Modified by: Jing Chen <jxc761@case.edu>
//              01/01/2015
// Copyright:   (c) 2015-2016 Jing Chen
// Licence:     
/////////////////////////////////////////////////////////////////////////////
//
// Notes: This backend is edited from following source code.
// 
// Here is origin source code information:
/////////////////////////////////////////////////////////////////////////////
// Name:        src/unix/mediactrl.cpp
// Purpose:     GStreamer backend for Unix
// Author:      Ryan Norton <wxprojects@comcast.net>
// Modified by:
// Created:     02/04/05
// Copyright:   (c) 2004-2005 Ryan Norton
// Licence:     wxWindows licence
/////////////////////////////////////////////////////////////////////////////
#include "GStreamerBackend.h"
//=============================================================================
// Implementation
//=============================================================================

IMPLEMENT_DYNAMIC_CLASS(CGStreamerMediaBackend, wxMediaBackend)

//-----------------------------------------------------------------------------
//
// C Callbacks
//
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// "expose_event" from m_ctrl->m_wxwindow
//
// Handle GTK expose event from our window - here we hopefully
// redraw the video in the case of pausing and other instances...
// (Returns TRUE to pass to other handlers, FALSE if not)
//
// TODO: Do a DEBUG_MAIN_THREAD/install_idle_handler here?
//-----------------------------------------------------------------------------
#ifdef __WXGTK__
extern "C" {
static gboolean
#ifdef __WXGTK3__
draw(GtkWidget* widget, cairo_t* cr, CGStreamerMediaBackend* be)
#else
expose_event(GtkWidget* widget, GdkEventExpose* event, CGStreamerMediaBackend* be)
#endif
{
    // I've seen this recommended somewhere...
    // TODO: Is this needed? Maybe it is just cruft...
    // gst_x_overlay_set_xwindow_id( GST_X_OVERLAY(be->m_xoverlay),
    //                              GDK_WINDOW_XWINDOW( window ) );

    // If we have actual video.....
    if(!(be->m_videoSize.x==0&&be->m_videoSize.y==0) &&
       GST_STATE(be->m_playbin) >= GST_STATE_PAUSED)
    {
        // GST Doesn't redraw automatically while paused
        // Plus, the video sometimes doesn't redraw when it looses focus
        // or is painted over so we just tell it to redraw...
        gst_x_overlay_expose(be->m_xoverlay);
    }
    else
    {
        // draw a black background like some other backends do....
#ifdef __WXGTK3__
        GtkAllocation a;
        gtk_widget_get_allocation(widget, &a);
        cairo_rectangle(cr, 0, 0, a.width, a.height);
        cairo_set_source_rgb(cr, 0, 0, 0);
        cairo_fill(cr);
#else
        gdk_draw_rectangle (event->window, widget->style->black_gc, TRUE, 0, 0,
                            widget->allocation.width,
                            widget->allocation.height);
#endif
    }

    return FALSE;
}
}
#endif // wxGTK

//-----------------------------------------------------------------------------
// "realize" from m_ctrl->m_wxwindow
//
// If the window wasn't realized when Load was called, this is the
// callback for when it is - the purpose of which is to tell
// GStreamer to play the video in our control
//-----------------------------------------------------------------------------
#ifdef __WXGTK__
extern "C" {
static gint gtk_window_realize_callback(GtkWidget* widget,
                                        CGStreamerMediaBackend* be)
{
    gdk_flush();

    GdkWindow* window = gtk_widget_get_window(widget);
    wxASSERT(window);

    gst_x_overlay_set_xwindow_id( GST_X_OVERLAY(be->m_xoverlay),
                                GDK_WINDOW_XID(window)
                                );
    GtkWidget* w = be->GetControl()->m_wxwindow;
#ifdef __WXGTK3__
    g_signal_connect(w, "draw", G_CALLBACK(draw), be);
#else
    g_signal_connect(w, "expose_event", G_CALLBACK(expose_event), be);
#endif
    return 0;
}
}
#endif // wxGTK

//-----------------------------------------------------------------------------
// "state-change" from m_playbin/GST_MESSAGE_STATE_CHANGE
//
// Called by gstreamer when the state changes - here we
// send the appropriate corresponding wx event.
//
// 0.8 only as HandleStateChange does this in both versions
//-----------------------------------------------------------------------------
#if GST_VERSION_MAJOR == 0 && GST_VERSION_MINOR < 10
extern "C" {
static void gst_state_change_callback(GstElement *play,
                                      GstElementState oldstate,
                                      GstElementState newstate,
                                      CGStreamerMediaBackend* be)
{
    if(be->m_asynclock.TryLock() == wxMUTEX_NO_ERROR)
    {
        be->HandleStateChange(oldstate, newstate);
        be->m_asynclock.Unlock();
    }
}
}
#endif // <0.10

//-----------------------------------------------------------------------------
// "eos" from m_playbin/GST_MESSAGE_EOS
//
// Called by gstreamer when the media is done playing ("end of stream")
//-----------------------------------------------------------------------------
extern "C" {
static void gst_finish_callback(GstElement *WXUNUSED(play),
                                CGStreamerMediaBackend* be)
{
    wxLogTrace(wxTRACE_GStreamer, wxT("gst_finish_callback"));
    wxMediaEvent event(wxEVT_MEDIA_FINISHED);
    be->m_eventHandler->AddPendingEvent(event);
}
}

//-----------------------------------------------------------------------------
// "error" from m_playbin/GST_MESSAGE_ERROR
//
// Called by gstreamer when an error is encountered playing the media -
// We call wxLogTrace in addition wxLogSysError so that we can get it
// on the command line as well for those who want extra traces.
//-----------------------------------------------------------------------------
extern "C" {
static void gst_error_callback(GstElement *WXUNUSED(play),
                               GstElement *WXUNUSED(src),
                               GError     *err,
                               gchar      *debug,
                               CGStreamerMediaBackend* be)
{
    wxMutexLocker lock(be->m_mutexErr);
    be->m_errors.push_back(CGStreamerMediaBackend::Error(err->message, debug));
}
}

//-----------------------------------------------------------------------------
// "notify::caps" from the videopad inside "stream-info" of m_playbin
//
// Called by gstreamer when the video caps for the media is ready - currently
// we use the caps to get the natural size of the video
//
// (Undocumented?)
//-----------------------------------------------------------------------------
extern "C" {
static void gst_notify_caps_callback(GstPad* pad,
                                     GParamSpec* WXUNUSED(pspec),
                                     CGStreamerMediaBackend* be)
{
    wxLogTrace(wxTRACE_GStreamer, wxT("gst_notify_caps_callback"));
    be->QueryVideoSizeFromPad(pad);
}
}

//-----------------------------------------------------------------------------
// "notify::stream-info" from m_playbin
//
// Run through the stuff in "stream-info" of m_playbin for a valid
// video pad, and then attempt to query the video size from it - if not
// set up an event to do so when ready.
//
// Currently unused - now we just query it directly using
// QueryVideoSizeFromElement.
//
// (Undocumented?)
//-----------------------------------------------------------------------------
#if GST_VERSION_MAJOR > 0 || GST_VERSION_MINOR >= 10
extern "C" {
static void gst_notify_stream_info_callback(GstElement* WXUNUSED(element),
                                            GParamSpec* WXUNUSED(pspec),
                                            CGStreamerMediaBackend* be)
{
    wxLogTrace(wxTRACE_GStreamer, wxT("gst_notify_stream_info_callback"));
    be->QueryVideoSizeFromElement(be->m_playbin);
}
}
#endif

//-----------------------------------------------------------------------------
// "desired-size-changed" from m_xoverlay
//
// 0.8-specific this provides us with the video size when it changes -
// even though we get the caps as well this seems to come before the
// caps notification does...
//
// Note it will return 16,16 for an early-bird value or for audio
//-----------------------------------------------------------------------------
#if GST_VERSION_MAJOR == 0 && GST_VERSION_MINOR < 10
extern "C" {
static void gst_desired_size_changed_callback(GstElement * play,
                                              guint width, guint height,
                                              CGStreamerMediaBackend* be)
{
    if(!(width == 16 && height == 16))
    {
        be->m_videoSize.x = width;
        be->m_videoSize.y = height;
    }
    else
        be->QueryVideoSizeFromElement(be->m_playbin);
}
}
#endif

//-----------------------------------------------------------------------------
// gst_bus_async_callback [static]
// gst_bus_sync_callback [static]
//
// Called by m_playbin for notifications such as end-of-stream in 0.10 -
// in previous versions g_signal notifications were used. Because everything
// in centered in one switch statement though it reminds one of old WinAPI
// stuff.
//
// gst_bus_sync_callback is that sync version that is called on the main GUI
// thread before the async version that we use to set the xwindow id of the
// XOverlay (NB: This isn't currently used - see CreateControl()).
//-----------------------------------------------------------------------------
#if GST_VERSION_MAJOR > 0 || GST_VERSION_MINOR >= 10
extern "C" {
static gboolean gst_bus_async_callback(GstBus* WXUNUSED(bus),
                                       GstMessage* message,
                                       CGStreamerMediaBackend* be)
{
    if ( GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR )
    {
        GError* error;
        gchar* debug;
        gst_message_parse_error(message, &error, &debug);
        gst_error_callback(NULL, NULL, error, debug, be);
        return FALSE;
    }

    if(((GstElement*)GST_MESSAGE_SRC(message)) != be->m_playbin)
        return TRUE;
    if(be->m_asynclock.TryLock() != wxMUTEX_NO_ERROR)
        return TRUE;

    switch(GST_MESSAGE_TYPE(message))
    {
        case GST_MESSAGE_STATE_CHANGED:
        {
            GstState oldstate, newstate, pendingstate;
            gst_message_parse_state_changed(message, &oldstate,
                                            &newstate, &pendingstate);
            be->HandleStateChange(oldstate, newstate);
            break;
        }
        case GST_MESSAGE_EOS:
        {
            gst_finish_callback(NULL, be);
            break;
        }

        default:
            break;
    }

    be->m_asynclock.Unlock();
    return FALSE; // remove the message from Z queue
}

static GstBusSyncReply gst_bus_sync_callback(GstBus* bus,
                                             GstMessage* message,
                                             CGStreamerMediaBackend* be)
{
		// handle prepare-window-handle element message
		if (gst_is_video_overlay_prepare_window_handle_message (message) )
		{
			wxLogTrace(wxTRACE_GStreamer, wxT("Got prepare-xwindow-id"));
    	be->SetupXOverlay();
    	return GST_BUS_DROP;
		}
		

    // Pass a non-xwindowid-setting event on to the async handler where it
    // belongs
    if (GST_MESSAGE_TYPE (message) != GST_MESSAGE_ELEMENT ||
        !gst_structure_has_name (gst_message_get_structure(message)/*message->structure*/, "prepare-xwindow-id"))
    {
        //
        // NB: Unfortunately, the async callback can be quite
        // buggy at times and often doesn't get called at all,
        // so here we are processing it right here in the calling
        // thread instead of the GUI one...
        //
        if(gst_bus_async_callback(bus, message, be))
            return GST_BUS_PASS;
        else
            return GST_BUS_DROP;
    }

    wxLogTrace(wxTRACE_GStreamer, wxT("Got prepare-xwindow-id"));
    be->SetupXOverlay();
    return GST_BUS_DROP; // We handled this message - drop from the queue
}
}
#endif


//-----------------------------------------------------------------------------
//
// Constructors
//
//-----------------------------------------------------------------------------


//-----------------------------------------------------------------------------
//CGStreamerMediaBackend::InitGstreamer()
//
//Initialize gstreamer
//
//Return false if failed
//-----------------------------------------------------------------------------
bool CGStreamerMediaBackend::InitGstreamer()
{
  

    //Convert arguments to unicode if enabled
#if wxUSE_UNICODE
    int i;
    char **argvGST = new char*[wxTheApp->argc + 1];
    for ( i = 0; i < wxTheApp->argc; i++ )
    {
        argvGST[i] = wxStrdupA(wxTheApp->argv[i].utf8_str());
    }

    argvGST[wxTheApp->argc] = NULL;

    int argcGST = wxTheApp->argc;
#else
#define argcGST wxTheApp->argc
#define argvGST wxTheApp->argv
#endif

    //Really init gstreamer
    gboolean bInited;
    GError* error = NULL;
#if GST_VERSION_MAJOR > 0 || GST_VERSION_MINOR >= 10
    bInited = gst_init_check(&argcGST, &argvGST, &error);
#else
    bInited = gst_init_check(&argcGST, &argvGST);
#endif

    // Cleanup arguments for unicode case
#if wxUSE_UNICODE
    for ( i = 0; i < argcGST; i++ )
    {
        free(argvGST[i]);
    }

    delete [] argvGST;
#endif

    if(!bInited)    //gst_init_check fail?
    {
        if(error)
        {
            wxLogSysError(wxT("Could not initialize GStreamer\n")
                          wxT("Error Message:%s"),
                          (const wxChar*) wxConvUTF8.cMB2WX(error->message)
                         );
            g_error_free(error);
        }
        else
            wxLogSysError(wxT("Could not initialize GStreamer"));

        return false;
    }
    return true;
  
}


bool CGStreamerMediaBackend::BuildPipeline()
{

    // Create our playbin object
    bool bInited = InitGstreamer();
    if (! bInited ){
      return false;
    }
    
    
    m_playbin = gst_element_factory_make ("playbin", "play");
    if (!GST_IS_ELEMENT(m_playbin))
    {
        if(G_IS_OBJECT(m_playbin))
            g_object_unref(m_playbin);
        wxLogSysError(wxT("Got an invalid playbin"));
        return false;
    }

#if GST_VERSION_MAJOR == 0 && GST_VERSION_MINOR < 10
    // Connect the glib events/callbacks we want to our playbin
    g_signal_connect(m_playbin, "eos",
                     G_CALLBACK(gst_finish_callback), this);
    g_signal_connect(m_playbin, "error",
                     G_CALLBACK(gst_error_callback), this);
    g_signal_connect(m_playbin, "state-change",
                     G_CALLBACK(gst_state_change_callback), this);
#else
    // GStreamer 0.10+ uses GstBus for this now, connect to the sync
    // handler as well so we can set the X window id of our xoverlay
    gst_bus_add_watch (gst_element_get_bus(m_playbin),
                       (GstBusFunc) gst_bus_async_callback, this);
    gst_bus_set_sync_handler(gst_element_get_bus(m_playbin),
                             (GstBusSyncHandler) gst_bus_sync_callback, this, NULL);
    g_signal_connect(m_playbin, "notify::stream-info",
                     G_CALLBACK(gst_notify_stream_info_callback), this);
#endif


    // Get the audio sink
    GstElement* audiosink = gst_gconf_get_default_audio_sink();
    if( !TryAudioSink(audiosink) )
    {
        // fallback to autodetection, then alsa, then oss as a stopgap
        audiosink = gst_element_factory_make ("autoaudiosink", "audio-sink");
        if( !TryAudioSink(audiosink) )
        {
            audiosink = gst_element_factory_make ("alsasink", "alsa-output");
            if( !TryAudioSink(audiosink) )
            {
                audiosink = gst_element_factory_make ("osssink", "play_audio");
                if( !TryAudioSink(audiosink) )
                {
                    wxLogSysError(wxT("Could not find a valid audiosink"));
                    return false;
                }
            }
        }
    }
    
    // Setup video sink
    GstElement* videosink = gst_element_factory_make("osxvideosink", "video-sink");
    if(videosink == NULL) {
    			g_object_unref(audiosink);
    			wxLogSysError(wxT("Could not make a native video sink"));
    			return false;
    }
    g_object_set(G_OBJECT(videosink), "embed", true, NULL);
    // set m_xoverlay
    m_xoverlay = (GstXOverlay*) videosink;
    gst_video_overlay_set_window_handle(m_xoverlay, (guintptr)m_ctrl->GetHandle());
    // SetupXOverlay();
    
    	/* comment out by Jing
     // Setup video sink - first try gconf, then auto, then xvimage and
    // then finally plain ximage
    GstElement* videosink = gst_gconf_get_default_video_sink();
    if( !TryVideoSink(videosink) )
    {
    	
    		videosink = gst_element_factory_make ("autovideosink", "video-sink");
      	
      	
        if( !TryVideoSink(videosink) )
        {
            videosink = gst_element_factory_make ("xvimagesink", "video-sink");
            if( !TryVideoSink(videosink) )
            {
                // finally, do a final fallback to ximagesink
                videosink =
                    gst_element_factory_make ("ximagesink", "video-sink");
                if( !TryVideoSink(videosink) )
                {
                    g_object_unref(audiosink);
                    wxLogSysError(wxT("Could not find a suitable video sink"));
                    return false;
                }
                
            }
        }
    }
    */


#if GST_VERSION_MAJOR == 0 && GST_VERSION_MINOR < 10
    // Not on 0.10... called when video size changes
    g_signal_connect(m_xoverlay, "desired-size-changed",
                     G_CALLBACK(gst_desired_size_changed_callback), this);
#endif
    // Tell GStreamer which window to draw to in 0.8 - 0.10
    // sometimes needs this too...
    SetupXOverlay();

    // Now that we know (or, rather think) our video and audio sink
    // are valid set our playbin to use them
    g_object_set (G_OBJECT (m_playbin),
                  "video-sink", videosink,
                  "audio-sink", audiosink,
                   NULL);
    return true;
}

//-----------------------------------------------------------------------------
// CGStreamerMediaBackend::CreateControl
//
// Initializes GStreamer and creates the wx side of our media control
//-----------------------------------------------------------------------------
bool CGStreamerMediaBackend::CreateControl(wxControl* ctrl, wxWindow* parent,
                                wxWindowID id,
                                const wxPoint& pos,
                                const wxSize& size,
                                long style,
                                const wxValidator& validator,
                                const wxString& name)
{
 
    //
    // wxControl creation
    //
    m_ctrl = wxStaticCast(ctrl, wxMediaCtrl);

#ifdef __WXGTK__
    // We handle our own GTK expose events
    m_ctrl->m_noExpose = true;
#endif

    if( !m_ctrl->wxControl::Create(parent, id, pos, size,
                            style,  // TODO: remove borders???
                            validator, name) )
    {
        wxFAIL_MSG(wxT("Could not create wxControl!!!"));
        return false;
    }

#ifdef __WXGTK__
    // Turn off double-buffering so that
    // so it doesn't draw over the video and cause sporadic
    // disappearances of the video
    gtk_widget_set_double_buffered(m_ctrl->m_wxwindow, FALSE);
#endif

    // don't erase the background of our control window
    // so that resizing is a bit smoother
    m_ctrl->SetBackgroundStyle(wxBG_STYLE_CUSTOM);


		//
		BuildPipeline();
		
    m_eventHandler = new CGStreamerMediaEventHandler(this);
    return true;
}


//-----------------------------------------------------------------------------
//
// Private (although not in the C++ sense)  methods
//
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// CGStreamerMediaBackend::HandleStateChange
//
// Handles a state change event from our C Callback for "state-change" or
// the async queue in 0.10. (Mostly this is here to avoid locking the
// the mutex twice...)
//-----------------------------------------------------------------------------
void CGStreamerMediaBackend::HandleStateChange(GstElementState oldstate,
                                                GstElementState newstate)
{
    switch(newstate)
    {
        case GST_STATE_PLAYING:
            wxLogTrace(wxTRACE_GStreamer, wxT("Play event"));
            QueuePlayEvent();
            break;
        case GST_STATE_PAUSED:
            // For some reason .10 sends a lot of oldstate == newstate
            // messages - most likely for pending ones - also
            // !<GST_STATE_PAUSED as we are only concerned
            if(oldstate < GST_STATE_PAUSED || oldstate == newstate)
                break;
            if(CGStreamerMediaBackend::GetPosition() != 0)
            {
                wxLogTrace(wxTRACE_GStreamer, wxT("Pause event"));
                QueuePauseEvent();
            }
            else
            {
                wxLogTrace(wxTRACE_GStreamer, wxT("Stop event"));
                QueueStopEvent();
            }
            break;
       default: // GST_STATE_NULL etc.
            break;
    }
}

//-----------------------------------------------------------------------------
// CGStreamerMediaBackend::QueryVideoSizeFromElement
//
// Run through the stuff in "stream-info" of element for a valid
// video pad, and then attempt to query the video size from it - if not
// set up an event to do so when ready. Return true
// if we got a valid video pad.
//-----------------------------------------------------------------------------
bool CGStreamerMediaBackend::QueryVideoSizeFromElement(GstElement* element)
{
    const GList *list = NULL;
    g_object_get (G_OBJECT (element), "stream-info", &list, NULL);

    for ( ; list != NULL; list = list->next)
    {
        GObject *info = (GObject *) list->data;
        gint type;
        GParamSpec *pspec;
        GEnumValue *val;
        GstPad *pad = NULL;

        g_object_get (info, "type", &type, NULL);
        pspec = g_object_class_find_property (
                        G_OBJECT_GET_CLASS (info), "type");
        val = g_enum_get_value (G_PARAM_SPEC_ENUM (pspec)->enum_class, type);

        if (!strncasecmp(val->value_name, "video", 5) ||
            !strncmp(val->value_name, "GST_STREAM_TYPE_VIDEO", 21))
        {
            // Newer gstreamer 0.8+ plugins are SUPPOSED to have "object"...
            // but a lot of old plugins still use "pad" :)
            pspec = g_object_class_find_property (
                        G_OBJECT_GET_CLASS (info), "object");

            if (!pspec)
                g_object_get (info, "pad", &pad, NULL);
            else
                g_object_get (info, "object", &pad, NULL);

#if GST_VERSION_MAJOR == 0 && GST_VERSION_MINOR <= 8
            // Killed in 0.9, presumely because events and such
            // should be pushed on pads regardless of whether they
            // are currently linked
            pad = (GstPad *) GST_PAD_REALIZE (pad);
            wxASSERT(pad);
#endif

            if(!QueryVideoSizeFromPad(pad))
            {
                // wait for those caps to get ready
                g_signal_connect(
                pad,
                "notify::caps",
                G_CALLBACK(gst_notify_caps_callback),
                this);
            }
            break;
        }// end if video
    }// end searching through info list

    // no video (or extremely delayed stream-info)
    if(list == NULL)
    {
        m_videoSize = wxSize(0,0);
        return false;
    }

    return true;
}

//-----------------------------------------------------------------------------
// CGStreamerMediaBackend::QueryVideoSizeFromPad
//
// Gets the size of our video (in wxSize) from a GstPad
//-----------------------------------------------------------------------------
bool CGStreamerMediaBackend::QueryVideoSizeFromPad(GstPad* pad)
{
    const GstCaps* caps = gst_pad_get_current_caps(pad); //GST_PAD_CAPS(pad);
    if ( caps )
    {
        const GstStructure *s = gst_caps_get_structure (caps, 0);
        wxASSERT(s);

        gst_structure_get_int (s, "width", &m_videoSize.x);
        gst_structure_get_int (s, "height", &m_videoSize.y);

        const GValue *par;
        par = gst_structure_get_value (s, "pixel-aspect-ratio");

        if (par)
        {
            wxLogTrace(wxTRACE_GStreamer,
                       wxT("pixel-aspect-ratio found in pad"));
            int num = par->data[0].v_int,
                den = par->data[1].v_int;

            // TODO: maybe better fraction normalization...
            if (num > den)
                m_videoSize.x = (int) ((float) num * m_videoSize.x / den);
            else
                m_videoSize.y = (int) ((float) den * m_videoSize.y / num);
        }

         wxLogTrace(wxTRACE_GStreamer, wxT("Adjusted video size: [%i,%i]"),
                     m_videoSize.x, m_videoSize.y);
        return true;
    } // end if caps

    return false; // not ready/massive failure
}


//-----------------------------------------------------------------------------
// CGStreamerMediaBackend::SetupXOverlay
//
// Attempts to set the XWindow id of our GstXOverlay to tell it which
// window to play video in.
//-----------------------------------------------------------------------------
void CGStreamerMediaBackend::SetupXOverlay()
{
	//
	gst_video_overlay_set_window_handle(GST_X_OVERLAY(m_xoverlay), (guintptr) m_ctrl->GetHandle());
	
	
	/*
    // Use the xoverlay extension to tell gstreamer to play in our window
#ifdef __WXGTK__
    if (!gtk_widget_get_realized(m_ctrl->m_wxwindow))
    {
        // Not realized yet - set to connect at realization time
        g_signal_connect (m_ctrl->m_wxwindow,
                          "realize",
                          G_CALLBACK (gtk_window_realize_callback),
                          this);
    }
    else
    {
        gdk_flush();

        GdkWindow* window = gtk_widget_get_window(m_ctrl->m_wxwindow);
        wxASSERT(window);
#endif
        gst_x_overlay_set_xwindow_id(GST_X_OVERLAY(m_xoverlay),
#ifdef __WXGTK__
                        GDK_WINDOW_XID(window)
#else
                        m_ctrl->GetId()
#endif
                                  );
#ifdef __WXGTK__
        GtkWidget* w = m_ctrl->m_wxwindow;
#ifdef __WXGTK3__
        g_signal_connect(w, "draw", G_CALLBACK(draw), this);
#else
        g_signal_connect(w, "expose_event", G_CALLBACK(expose_event), this);
#endif
    } // end if GtkPizza realized
#endif
*/

}

//-----------------------------------------------------------------------------
// CGStreamerMediaBackend::SyncStateChange
//
// This function is rather complex - basically the idea is that we
// poll the GstBus of m_playbin until it has reached desiredstate, an error
// is reached, or there are no more messages left in the GstBus queue.
//
// Returns true if there are no messages left in the queue or
// the current state reaches the disired state.
//
// PRECONDITION: Assumes m_asynclock is Lock()ed
//-----------------------------------------------------------------------------
#if GST_VERSION_MAJOR > 0 || GST_VERSION_MINOR >= 10
bool CGStreamerMediaBackend::SyncStateChange(GstElement* element,
                                              GstElementState desiredstate,
                                              gint64 llTimeout)
{
    GstBus* bus = gst_element_get_bus(element);
    GstMessage* message;
    bool bBreak = false,
         bSuccess = false;
    gint64 llTimeWaited = 0;

    do
    {
#if 1
        // NB: The GStreamer gst_bus_poll is unfortunately broken and
        // throws silly critical internal errors (for instance
        // "message != NULL" when the whole point of it is to
        // poll for the message in the first place!) so we implement
        // our own "waiting mechinism"
        if(gst_bus_have_pending(bus) == FALSE)
        {
            if(llTimeWaited >= llTimeout)
                return true; // Reached timeout... assume success
            llTimeWaited += 10*GST_MSECOND;
            wxMilliSleep(10);
            continue;
        }

        message = gst_bus_pop(bus);
#else
        message = gst_bus_poll(bus, (GstMessageType)
                           (GST_MESSAGE_STATE_CHANGED |
                            GST_MESSAGE_ERROR |
                            GST_MESSAGE_EOS), llTimeout);
        if(!message)
            return true;
#endif
        if(((GstElement*)GST_MESSAGE_SRC(message)) == element)
        {
            switch(GST_MESSAGE_TYPE(message))
            {
                case GST_MESSAGE_STATE_CHANGED:
                {
                    GstState oldstate, newstate, pendingstate;
                    gst_message_parse_state_changed(message, &oldstate,
                                                    &newstate, &pendingstate);
                    if(newstate == desiredstate)
                    {
                        bSuccess = bBreak = true;
                    }
                    break;
                }
                case GST_MESSAGE_ERROR:
                {
                    GError* error;
                    gchar* debug;
                    gst_message_parse_error(message, &error, &debug);
                    gst_error_callback(NULL, NULL, error, debug, this);
                    bBreak = true;
                    break;
                }
                case GST_MESSAGE_EOS:
                    wxLogSysError(wxT("Reached end of stream prematurely"));
                    bBreak = true;
                    break;
                default:
                    break; // not handled
            }
        }

        gst_message_unref(message);
    }while(!bBreak);

    return bSuccess;
}
#else // 0.8 implementation
bool CGStreamerMediaBackend::SyncStateChange(GstElement* element,
                                              GstElementState desiredstate,
                                              gint64 llTimeout)
{
    gint64 llTimeWaited = 0;
    while(GST_STATE(element) != desiredstate)
    {
        if(llTimeWaited >= llTimeout)
            break;
        llTimeWaited += 10*GST_MSECOND;
        wxMilliSleep(10);
    }

    return llTimeWaited != llTimeout;
}
#endif

//-----------------------------------------------------------------------------
// CGStreamerMediaBackend::TryAudioSink
// CGStreamerMediaBackend::TryVideoSink
//
// Uses various means to determine whether a passed in video/audio sink
// if suitable for us - if it is not we return false and unref the
// inappropriate sink.
//-----------------------------------------------------------------------------
bool CGStreamerMediaBackend::TryAudioSink(GstElement* audiosink)
{
    if( !GST_IS_ELEMENT(audiosink) )
    {
        if(G_IS_OBJECT(audiosink))
            g_object_unref(audiosink);
        return false;
    }

    return true;
}



bool CGStreamerMediaBackend::TryVideoSink(GstElement* videosink)
{
	  // Check if the video sink either is an xoverlay or might contain one...
    if( !GST_IS_BIN(videosink) && !GST_IS_X_OVERLAY(videosink) )
    {
        if(G_IS_OBJECT(videosink))
            g_object_unref(videosink);
        return false;
    }

    // Make our video sink and make sure it supports the x overlay interface
    // the x overlay enables us to put the video in our control window
    // (i.e. we NEED it!) - also connect to the natural video size change event
    if( GST_IS_BIN(videosink) )
        m_xoverlay = (GstXOverlay*)
                        gst_bin_get_by_interface (GST_BIN (videosink),
                                                  GST_TYPE_X_OVERLAY);
    else
        m_xoverlay = (GstXOverlay*) videosink;

    if ( !GST_IS_X_OVERLAY(m_xoverlay) )
    {
        g_object_unref(videosink);
        return false;
    }

    return true;

}

//-----------------------------------------------------------------------------
// CGStreamerMediaEventHandler::OnMediaFinish
//
// Called when the media is about to stop
//-----------------------------------------------------------------------------
void CGStreamerMediaEventHandler::OnMediaFinish(wxMediaEvent& WXUNUSED(event))
{
    // (RN - I have no idea why I thought this was good behaviour....
    // maybe it made sense for streaming/nonseeking data but
    // generally it seems like a really bad idea) -
    if(m_be->SendStopEvent())
    {
        // Stop the media (we need to set it back to paused
        // so that people can get the duration et al.
        // and send the finish event (luckily we can "Sync" it out... LOL!)
        // (We don't check return values here because we can't really do
        //  anything...)
        wxMutexLocker lock(m_be->m_asynclock);

        // Set element to ready+sync it
        gst_element_set_state (m_be->m_playbin, GST_STATE_READY);
        m_be->SyncStateChange(m_be->m_playbin, GST_STATE_READY);

        // Now set it to paused + update pause pos to 0 and
        // Sync that as well (note that we don't call Stop() here
        // due to mutex issues)
        gst_element_set_state (m_be->m_playbin, GST_STATE_PAUSED);
        m_be->SyncStateChange(m_be->m_playbin, GST_STATE_PAUSED);
        m_be->m_llPausedPos = 0;

        // Finally, queue the finish event
        m_be->QueueFinishEvent();
    }
}

//-----------------------------------------------------------------------------
//
// Public methods
//
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// CGStreamerMediaBackend Constructor
//
// Sets m_playbin to NULL signifying we havn't loaded anything yet
//-----------------------------------------------------------------------------
CGStreamerMediaBackend::CGStreamerMediaBackend()
    : m_playbin(NULL),
      m_eventHandler(NULL)
{
}

//-----------------------------------------------------------------------------
// CGStreamerMediaBackend Destructor
//
// Stops/cleans up memory
//
// NB: This could trigger a critical warning but doing a SyncStateChange
//     here is just going to slow down quitting of the app, which is bad.
//-----------------------------------------------------------------------------
CGStreamerMediaBackend::~CGStreamerMediaBackend()
{
    // Dispose of the main player and related objects
    if(m_playbin)
    {
        wxASSERT( GST_IS_OBJECT(m_playbin) );
        gst_element_set_state (m_playbin, GST_STATE_NULL);
        gst_object_unref (GST_OBJECT (m_playbin));
        delete m_eventHandler;
    }
}

//-----------------------------------------------------------------------------
// CGStreamerMediaBackend::CheckForErrors
//
// Reports any errors received from gstreamer. Should be called after any
// failure.
//-----------------------------------------------------------------------------
bool CGStreamerMediaBackend::CheckForErrors()
{
    wxMutexLocker lock(m_mutexErr);
    if ( m_errors.empty() )
        return false;

    for ( unsigned n = 0; n < m_errors.size(); n++ )
    {
        const Error& err = m_errors[n];

        wxLogTrace(wxTRACE_GStreamer,
                   "gst_error_callback: %s", err.m_debug);
        wxLogError(_("Media playback error: %s"), err.m_message);
    }

    m_errors.clear();

    return true;
}



//-----------------------------------------------------------------------------
// CGStreamerMediaBackend::Load (File version)
//
// Just calls DoLoad() with a prepended file scheme
//-----------------------------------------------------------------------------
bool CGStreamerMediaBackend::Load(const wxString& fileName)
{
    return DoLoad(wxFileSystem::FileNameToURL(fileName));
}

//-----------------------------------------------------------------------------
// CGStreamerMediaBackend::Load (URI version)
//
// In the case of a file URI passes it unencoded -
// also, as of 0.10.3 and earlier GstURI (the uri parser for gstreamer)
// is sort of broken and only accepts uris with at least two slashes
// after the scheme (i.e. file: == not ok, file:// == ok)
//-----------------------------------------------------------------------------
bool CGStreamerMediaBackend::Load(const wxURI& location)
{
    if(location.GetScheme().CmpNoCase(wxT("file")) == 0)
    {
        wxString uristring = location.BuildUnescapedURI();

        //Workaround GstURI leading "//" problem and make sure it leads
        //with that
        return DoLoad(wxString(wxT("file://")) +
                      uristring.Right(uristring.length() - 5)
                     );
    }
    else
        return DoLoad(location.BuildURI());
}

//-----------------------------------------------------------------------------
// CGStreamerMediaBackend::DoLoad
//
// Loads the media
// 1) Reset member variables and set playbin back to ready state
// 2) Check URI for validity and then tell the playbin to load it
// 3) Set the playbin to the pause state
//
// NB: Even after this function is over with we probably don't have the
// video size or duration - no amount of clever hacking is going to get
// around that, unfortunately.
//-----------------------------------------------------------------------------
bool CGStreamerMediaBackend::DoLoad(const wxString& locstring)
{
		//wxMessageBox(locstring, "loading");
    wxMutexLocker lock(m_asynclock); // lock state events and async callbacks

    // Reset positions & rate
    m_llPausedPos = 0;
    m_dRate = 1.0;
    m_videoSize = wxSize(0,0);

    // Set playbin to ready to stop the current media...
    if( gst_element_set_state (m_playbin,
                               GST_STATE_READY) == GST_STATE_FAILURE ||
        !SyncStateChange(m_playbin, GST_STATE_READY))
    {
        CheckForErrors();

        wxLogError(_("Failed to prepare playing \"%s\"."), locstring);
        return false;
    }

    // free current media resources
    gst_element_set_state (m_playbin, GST_STATE_NULL);

    // Make sure the passed URI is valid and tell playbin to load it
    // non-file uris are encoded
    wxASSERT(gst_uri_protocol_is_valid("file"));
    wxASSERT(gst_uri_is_valid(locstring.mb_str()));

    g_object_set (G_OBJECT (m_playbin), "uri",
                  (const char*)locstring.mb_str(), NULL);

    // Try to pause media as gstreamer won't let us query attributes
    // such as video size unless it is paused or playing
    if( gst_element_set_state (m_playbin,
                               GST_STATE_PAUSED) == GST_STATE_FAILURE ||
        !SyncStateChange(m_playbin, GST_STATE_PAUSED))
    {
        CheckForErrors();
        return false; // no real error message needed here as this is
                      // generic failure 99% of the time (i.e. no
                      // source etc.) and has an error message
    }

    // It may happen that both calls above succeed but we actually had some
    // errors during the pipeline setup and it doesn't play. E.g. this happens
    // if XVideo extension is unavailable but xvimagesink is still used.
    if ( CheckForErrors() )
        return false;


    NotifyMovieLoaded(); // Notify the user - all we can do for now
    return true;
}


//-----------------------------------------------------------------------------
// CGStreamerMediaBackend::Play
//
// Sets the stream to a playing state
//
// THREAD-UNSAFE in 0.8, maybe in 0.10 as well
//-----------------------------------------------------------------------------
bool CGStreamerMediaBackend::Play()
{
    if (gst_element_set_state (m_playbin,
                               GST_STATE_PLAYING) == GST_STATE_FAILURE)
    {
        CheckForErrors();
        return false;
    }

    return true;
}

//-----------------------------------------------------------------------------
// CGStreamerMediaBackend::Pause
//
// Marks where we paused and pauses the stream
//
// THREAD-UNSAFE in 0.8, maybe in 0.10 as well
//-----------------------------------------------------------------------------
bool CGStreamerMediaBackend::Pause()
{
    m_llPausedPos = CGStreamerMediaBackend::GetPosition();
    if (gst_element_set_state (m_playbin,
                               GST_STATE_PAUSED) == GST_STATE_FAILURE)
    {
        CheckForErrors();
        return false;
    }
    return true;
}

//-----------------------------------------------------------------------------
// CGStreamerMediaBackend::Stop
//
// Pauses the stream and sets the position to 0. Note that this is
// synchronous (!) pausing.
//
// Due to the mutex locking this is probably thread-safe actually.
//-----------------------------------------------------------------------------
bool CGStreamerMediaBackend::Stop()
{
    {   // begin state lock
        wxMutexLocker lock(m_asynclock);
        if(gst_element_set_state (m_playbin,
                                  GST_STATE_PAUSED) == GST_STATE_FAILURE ||
          !SyncStateChange(m_playbin, GST_STATE_PAUSED))
        {
            CheckForErrors();
            wxLogSysError(wxT("Could not set state to paused for Stop()"));
            return false;
        }
    }   // end state lock

    bool bSeekedOK = CGStreamerMediaBackend::SetPosition(0);

    if(!bSeekedOK)
    {
        wxLogSysError(wxT("Could not seek to initial position in Stop()"));
        return false;
    }

    QueueStopEvent(); // Success
    return true;
}

//-----------------------------------------------------------------------------
// CGStreamerMediaBackend::GetState
//
// Gets the state of the media
//-----------------------------------------------------------------------------
wxMediaState CGStreamerMediaBackend::GetState()
{
    switch(GST_STATE(m_playbin))
    {
        case GST_STATE_PLAYING:
            return wxMEDIASTATE_PLAYING;
        case GST_STATE_PAUSED:
            if (m_llPausedPos == 0)
                return wxMEDIASTATE_STOPPED;
            else
                return wxMEDIASTATE_PAUSED;
        default://case GST_STATE_READY:
            return wxMEDIASTATE_STOPPED;
    }
}

//-----------------------------------------------------------------------------
// CGStreamerMediaBackend::GetPosition
//
// If paused, returns our marked position - otherwise it queries the
// GStreamer playbin for the position and returns that
//
// NB:
// NB: At least in 0.8, when you pause and seek gstreamer
// NB: doesn't update the position sometimes, so we need to keep track of
// NB: whether we have paused or not and keep track of the time after the
// NB: pause and whenever the user seeks while paused
// NB:
//
// THREAD-UNSAFE, at least if not paused. Requires media to be at least paused.
//-----------------------------------------------------------------------------
wxLongLong CGStreamerMediaBackend::GetPosition()
{
    if(GetState() != wxMEDIASTATE_PLAYING)
        return m_llPausedPos;
    else
    {
        gint64 pos;
        GstFormat fmtTime = GST_FORMAT_TIME;

        if (!gst_element_query_position(m_playbin, fmtTime, &pos) ||
            fmtTime != GST_FORMAT_TIME || pos == -1)
            return 0;
        return pos / GST_MSECOND ;
    }
}

//-----------------------------------------------------------------------------
// CGStreamerMediaBackend::SetPosition
//
// Sets the position of the stream
// Note that GST_MSECOND is 1000000 (GStreamer uses nanoseconds - so
// there is 1000000 nanoseconds in a millisecond)
//
// If we are paused we update the cached pause position.
//
// This is also an exceedingly ugly function due to the three implementations
// (or, rather two plus one implementation without a seek function).
//
// This is asynchronous and thread-safe on both 0.8 and 0.10.
//
// NB: This fires both a stop and play event if the media was previously
// playing... which in some ways makes sense. And yes, this makes the video
// go all haywire at times - a gstreamer bug...
//-----------------------------------------------------------------------------
bool CGStreamerMediaBackend::SetPosition(wxLongLong where)
{
#if GST_VERSION_MAJOR == 0 && GST_VERSION_MINOR == 8 \
                           && GST_VERSION_MICRO == 0
    // 0.8.0 has no gst_element_seek according to official docs!!!
    wxLogSysError(wxT("GStreamer 0.8.0 does not have gst_element_seek")
                  wxT(" according to official docs"));
    return false;
#else // != 0.8.0

#   if GST_VERSION_MAJOR > 0 || GST_VERSION_MINOR >= 10
        gst_element_seek (m_playbin, m_dRate, GST_FORMAT_TIME,
           (GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
                          GST_SEEK_TYPE_SET, where.GetValue() * GST_MSECOND,
                          GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE );
#   else
        // NB: Some gstreamer versions return false basically all the time
        // here - even totem doesn't bother to check the return value here
        // so I guess we'll just assume it worked -
        // TODO: maybe check the gst error callback???
        gst_element_seek (m_playbin, (GstSeekType) (GST_SEEK_METHOD_SET |
            GST_FORMAT_TIME | GST_SEEK_FLAG_FLUSH),
            where.GetValue() * GST_MSECOND );

#   endif // GST_VERSION_MAJOR > 0 || GST_VERSION_MINOR >= 10

    {
        m_llPausedPos = where;
        return true;
    }
    return true;
#endif //== 0.8.0
}

//-----------------------------------------------------------------------------
// CGStreamerMediaBackend::GetDuration
//
// Obtains the total time of our stream
// THREAD-UNSAFE, requires media to be paused or playing
//-----------------------------------------------------------------------------
wxLongLong CGStreamerMediaBackend::GetDuration()
{
    gint64 length;
    GstFormat fmtTime = GST_FORMAT_TIME;

    if(!gst_element_query_duration(m_playbin, fmtTime, &length) ||
       fmtTime != GST_FORMAT_TIME || length == -1)
        return 0;
    return length / GST_MSECOND ;
}

//-----------------------------------------------------------------------------
// CGStreamerMediaBackend::Move
//
// Called when the window is moved - GStreamer takes care of this
// for us so nothing is needed
//-----------------------------------------------------------------------------
void CGStreamerMediaBackend::Move(int WXUNUSED(x),
                                   int WXUNUSED(y),
                                   int WXUNUSED(w),
                                   int WXUNUSED(h))
{
}

//-----------------------------------------------------------------------------
// CGStreamerMediaBackend::GetVideoSize
//
// Returns our cached video size from Load/gst_notify_caps_callback
// gst_x_overlay_get_desired_size also does this in 0.8...
//-----------------------------------------------------------------------------
wxSize CGStreamerMediaBackend::GetVideoSize() const
{
    return m_videoSize;
}

//-----------------------------------------------------------------------------
// CGStreamerMediaBackend::GetPlaybackRate
// CGStreamerMediaBackend::SetPlaybackRate
//
// Obtains/Sets the playback rate of the stream
//
//TODO: PlaybackRate not currently supported via playbin directly -
//TODO: Ronald S. Bultje noted on gstreamer-devel:
//TODO:
//TODO: Like "play at twice normal speed"? Or "play at 25 fps and 44,1 kHz"? As
//TODO: for the first, yes, we have elements for that, btu they"re not part of
//TODO: playbin. You can create a bin (with a ghost pad) containing the actual
//TODO: video/audiosink and the speed-changing element for this, and set that
//TODO: element as video-sink or audio-sink property in playbin. The
//TODO: audio-element is called "speed", the video-element is called "videodrop"
//TODO: (although that appears to be deprecated in favour of "videorate", which
//TODO: again cannot do this, so this may not work at all in the end). For
//TODO: forcing frame/samplerates, see audioscale and videorate. Audioscale is
//TODO: part of playbin.
//
// In 0.10 GStreamer has new gst_element_seek API that might
// support this - and I've got an attempt to do so but it is untested
// but it would appear to work...
//-----------------------------------------------------------------------------
double CGStreamerMediaBackend::GetPlaybackRate()
{
    return m_dRate; // Could use GST_QUERY_RATE but the API doesn't seem
                    // final on that yet and there may not be any actual
                    // plugins that support it...
}

bool CGStreamerMediaBackend::SetPlaybackRate(double dRate)
{
#if GST_VERSION_MAJOR > 0 || GST_VERSION_MINOR >= 10
#if 0 // not tested enough
    if( gst_element_seek (m_playbin, dRate, GST_FORMAT_TIME,
                 (GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
                          GST_SEEK_TYPE_CUR, 0,
                          GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE ) )
    {
        m_dRate = dRate;
        return true;
    }
#else
    wxUnusedVar(dRate);
#endif
#endif

    // failure
    return false;
}

//-----------------------------------------------------------------------------
// CGStreamerMediaBackend::GetDownloadProgress
//
// Not really outwardly possible - have been suggested that one could
// get the information from the component that "downloads"
//-----------------------------------------------------------------------------
wxLongLong CGStreamerMediaBackend::GetDownloadProgress()
{
    return 0;
}

//-----------------------------------------------------------------------------
// CGStreamerMediaBackend::GetDownloadTotal
//
// TODO: Cache this?
// NB: The length changes every call for some reason due to
//     GStreamer implementation issues
// THREAD-UNSAFE, requires media to be paused or playing
//-----------------------------------------------------------------------------
wxLongLong CGStreamerMediaBackend::GetDownloadTotal()
{
    gint64 length;
    GstFormat fmtBytes = GST_FORMAT_BYTES;

    if (!gst_element_query_duration(m_playbin, fmtBytes, &length) ||
          fmtBytes != GST_FORMAT_BYTES || length == -1)
        return 0;
    return length;
}

//-----------------------------------------------------------------------------
// CGStreamerMediaBackend::SetVolume
// CGStreamerMediaBackend::GetVolume
//
// Sets/Gets the volume through the playbin object.
// Note that this requires a relatively recent gst-plugins so we
// check at runtime to see whether it is available or not otherwise
// GST spits out an error on the command line
//-----------------------------------------------------------------------------
bool CGStreamerMediaBackend::SetVolume(double dVolume)
{
    if(g_object_class_find_property(
            G_OBJECT_GET_CLASS(G_OBJECT(m_playbin)),
            "volume") != NULL)
    {
        g_object_set(G_OBJECT(m_playbin), "volume", dVolume, NULL);
        return true;
    }
    else
    {
        wxLogTrace(wxTRACE_GStreamer,
            wxT("SetVolume: volume prop not found - 0.8.5 of ")
            wxT("gst-plugins probably needed"));
    return false;
    }
}

double CGStreamerMediaBackend::GetVolume()
{
    double dVolume = 1.0;

    if(g_object_class_find_property(
            G_OBJECT_GET_CLASS(G_OBJECT(m_playbin)),
            "volume") != NULL)
    {
        g_object_get(G_OBJECT(m_playbin), "volume", &dVolume, NULL);
    }
    else
    {
        wxLogTrace(wxTRACE_GStreamer,
            wxT("GetVolume: volume prop not found - 0.8.5 of ")
            wxT("gst-plugins probably needed"));
    }

    return dVolume;
}



// Force link into main library so this backend can be loaded
#include "wx/html/forcelnk.h"
FORCE_LINK_ME(basewxmediabackends)

