/*
 * GStreamer
 * Copyright (C) 2020 Matthew Waters <matthew@centricular.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

/**
 * SECTION:gstqtoverlay
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstqtoverlay.h"
#include "qtglrenderer.h"

#include <gst/gl/gstglfuncs.h>

#define GST_CAT_DEFAULT gst_debug_qt_gl_overlay
GST_DEBUG_CATEGORY (GST_CAT_DEFAULT);

static void gst_qt_overlay_finalize (GObject * object);
static void gst_qt_overlay_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * param_spec);
static void gst_qt_overlay_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * param_spec);

static gboolean gst_qt_overlay_gl_start (GstGLBaseFilter * bfilter);
static void gst_qt_overlay_gl_stop (GstGLBaseFilter * bfilter);
static gboolean gst_qt_overlay_gl_set_caps (GstGLBaseFilter * bfilter,
    GstCaps * in_caps, GstCaps * out_caps);

static GstFlowReturn gst_qt_overlay_prepare_output_buffer (GstBaseTransform * btrans,
    GstBuffer * buffer, GstBuffer ** outbuf);
static GstFlowReturn gst_qt_overlay_transform (GstBaseTransform * btrans,
    GstBuffer * inbuf, GstBuffer * outbuf);

enum
{
  PROP_0,
  PROP_WIDGET,
  PROP_QML_SCENE,
};

enum
{
  SIGNAL_0,
  SIGNAL_QML_SCENE_INITIALIZED,
  LAST_SIGNAL
};

static guint gst_qt_overlay_signals[LAST_SIGNAL] = { 0 };

#define gst_qt_overlay_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (GstQtOverlay, gst_qt_overlay,
    GST_TYPE_GL_FILTER, GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT,
        "qtoverlay", 0, "Qt Video Overlay"));

static void
gst_qt_overlay_class_init (GstQtOverlayClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;
  GstBaseTransformClass *btrans_class;
  GstGLBaseFilterClass *glbasefilter_class;
  GstGLFilterClass *glfilter_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;
  glbasefilter_class = (GstGLBaseFilterClass *) klass;
  glfilter_class = (GstGLFilterClass *) klass;
  btrans_class = (GstBaseTransformClass *) klass;

  gobject_class->set_property = gst_qt_overlay_set_property;
  gobject_class->get_property = gst_qt_overlay_get_property;
  gobject_class->finalize = gst_qt_overlay_finalize;

  gst_element_class_set_metadata (gstelement_class, "Qt Video Overlay",
      "Filter/QML/Overlay", "A filter that renders a QML scene onto a video stream",
      "Matthew Waters <matthew@centricular.com>");

  g_object_class_install_property (gobject_class, PROP_QML_SCENE,
      g_param_spec_string ("qml-scene", "QML Scene",
          "The contents of the QML scene", NULL,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject_class, PROP_WIDGET,
      g_param_spec_pointer ("widget", "QQuickItem",
          "The QQuickItem to place the input video in the object hierarchy",
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  /**
   * GstQmlGLOverlay::qml-scene-initialized
   * @element: the #GstQmlGLOverlay
   * @root_item: the `QQuickItem` found to be the root item
   * @user_data: user provided data
   */
  gst_qt_overlay_signals[SIGNAL_QML_SCENE_INITIALIZED] =
      g_signal_new ("qml-scene-initialized", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_POINTER);

  gst_gl_filter_add_rgba_pad_templates (glfilter_class);

  btrans_class->prepare_output_buffer = gst_qt_overlay_prepare_output_buffer;
  btrans_class->transform = gst_qt_overlay_transform;

  glbasefilter_class->gl_start = gst_qt_overlay_gl_start;
  glbasefilter_class->gl_stop = gst_qt_overlay_gl_stop;
  glbasefilter_class->gl_set_caps = gst_qt_overlay_gl_set_caps;
}

static void
gst_qt_overlay_init (GstQtOverlay * qt_overlay)
{
  qt_overlay->widget = QSharedPointer<QtGLVideoItemInterface>();
}

static void
gst_qt_overlay_finalize (GObject * object)
{
  GstQtOverlay *qt_overlay = GST_QT_OVERLAY (object);

  qt_overlay->widget.clear();

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_qt_overlay_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstQtOverlay *qt_overlay = GST_QT_OVERLAY (object);

  switch (prop_id) {
    case PROP_WIDGET: {
      QtGLVideoItem *qt_item = static_cast<QtGLVideoItem *> (g_value_get_pointer (value));
      if (qt_item)
        qt_overlay->widget = qt_item->getInterface();
      else
        qt_overlay->widget.clear();
      break;
    }
    case PROP_QML_SCENE:
      g_free (qt_overlay->qml_scene);
      qt_overlay->qml_scene = g_value_dup_string (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_qt_overlay_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstQtOverlay *qt_overlay = GST_QT_OVERLAY (object);

  switch (prop_id) {
    case PROP_WIDGET:
      /* This is not really safe - the app needs to be
       * sure the widget is going to be kept alive or
       * this can crash */
      if (qt_overlay->widget)
        g_value_set_pointer (value, qt_overlay->widget->videoItem());
      else
        g_value_set_pointer (value, NULL);
      break;
    case PROP_QML_SCENE:
      g_value_set_string (value, qt_overlay->qml_scene);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static gboolean
gst_qt_overlay_gl_start (GstGLBaseFilter * bfilter)
{
  GstQtOverlay *qt_overlay = GST_QT_OVERLAY (bfilter);
  QQuickItem *root;
  GError *error = NULL;

  GST_TRACE_OBJECT (bfilter, "using scene:\n%s", qt_overlay->qml_scene);

  if (!qt_overlay->qml_scene || g_strcmp0 (qt_overlay->qml_scene, "") == 0) {
    GST_ELEMENT_ERROR (bfilter, RESOURCE, NOT_FOUND, ("qml-scene property not set"), (NULL));
    return FALSE;
  }

  if (!GST_GL_BASE_FILTER_CLASS (parent_class)->gl_start (bfilter))
    return FALSE;

  qt_overlay->renderer = new GstQuickRenderer;
  if (!qt_overlay->renderer->init (bfilter->context, &error)) {
    GST_ELEMENT_ERROR (GST_ELEMENT (bfilter), RESOURCE, NOT_FOUND,
        ("%s", error->message), (NULL));
    return FALSE;
  }
  /* FIXME: Qml may do async loading and we need to propagate qml errors in that case as well */
  if (!qt_overlay->renderer->setQmlScene (qt_overlay->qml_scene, &error)) {
    GST_ELEMENT_ERROR (GST_ELEMENT (bfilter), RESOURCE, NOT_FOUND,
        ("%s", error->message), (NULL));
    return FALSE;
  }

  root = qt_overlay->renderer->rootItem();
  if (!root) {
    GST_ELEMENT_ERROR (GST_ELEMENT (bfilter), RESOURCE, NOT_FOUND,
        ("Qml scene does not have a root item"), (NULL));
    return FALSE;
  }

  g_signal_emit (qt_overlay, gst_qt_overlay_signals[SIGNAL_QML_SCENE_INITIALIZED], 0, root);

  return TRUE;
}

static void
gst_qt_overlay_gl_stop (GstGLBaseFilter * bfilter)
{
  GstQtOverlay *qt_overlay = GST_QT_OVERLAY (bfilter);

  if (qt_overlay->widget)
    qt_overlay->widget->setBuffer (NULL);

  if (qt_overlay->renderer) {
    qt_overlay->renderer->cleanup();
    delete qt_overlay->renderer;
  }
  qt_overlay->renderer = NULL;

  GST_GL_BASE_FILTER_CLASS (parent_class)->gl_stop (bfilter);
}

static gboolean
gst_qt_overlay_gl_set_caps (GstGLBaseFilter * bfilter, GstCaps * in_caps,
    GstCaps * out_caps)
{
  GstGLFilter *filter = GST_GL_FILTER (bfilter);
  GstQtOverlay *qt_overlay = GST_QT_OVERLAY (bfilter);

  if (!GST_GL_BASE_FILTER_CLASS (parent_class)->gl_set_caps (bfilter, in_caps, out_caps))
    return FALSE;

  qt_overlay->renderer->setSize (GST_VIDEO_INFO_WIDTH (&filter->out_info),
      GST_VIDEO_INFO_HEIGHT (&filter->out_info));

  return TRUE;
}

static GstFlowReturn
gst_qt_overlay_prepare_output_buffer (GstBaseTransform * btrans,
    GstBuffer * buffer, GstBuffer ** outbuf)
{
  GstBaseTransformClass *bclass = GST_BASE_TRANSFORM_GET_CLASS (btrans);
  GstGLBaseFilter *bfilter = GST_GL_BASE_FILTER (btrans);
  GstGLFilter *filter = GST_GL_FILTER (btrans);
  GstQtOverlay *qt_overlay = GST_QT_OVERLAY (btrans);
  GstGLMemory *out_mem;
  GstGLSyncMeta *sync_meta;

  if (qt_overlay->widget) {
    qt_overlay->widget->setCaps (bfilter->in_caps); 
    qt_overlay->widget->setBuffer (buffer);
  }

  /* XXX: is this the correct ts to drive the animation */
  out_mem = qt_overlay->renderer->generateOutput (GST_BUFFER_PTS (buffer));
  if (!out_mem) {
    GST_ERROR_OBJECT (qt_overlay, "Failed to generate output");
    return GST_FLOW_ERROR;
  }

  *outbuf = gst_buffer_new ();
  gst_buffer_append_memory (*outbuf, (GstMemory *) out_mem);
  gst_buffer_add_video_meta (*outbuf, (GstVideoFrameFlags) 0,
      GST_VIDEO_INFO_FORMAT (&filter->out_info),
      GST_VIDEO_INFO_WIDTH (&filter->in_info),
      GST_VIDEO_INFO_HEIGHT (&filter->out_info));

  sync_meta = gst_buffer_add_gl_sync_meta (bfilter->context, *outbuf);
  gst_gl_sync_meta_set_sync_point (sync_meta, bfilter->context);

  bclass->copy_metadata (btrans, buffer, *outbuf);

  return GST_FLOW_OK;
}

static GstFlowReturn
gst_qt_overlay_transform (GstBaseTransform * btrans, GstBuffer * inbuf,
    GstBuffer * outbuf)
{
  return GST_FLOW_OK;
}