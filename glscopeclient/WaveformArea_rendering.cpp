/***********************************************************************************************************************
*                                                                                                                      *
* ANTIKERNEL v0.1                                                                                                      *
*                                                                                                                      *
* Copyright (c) 2012-2020 Andrew D. Zonenberg                                                                          *
* All rights reserved.                                                                                                 *
*                                                                                                                      *
* Redistribution and use in source and binary forms, with or without modification, are permitted provided that the     *
* following conditions are met:                                                                                        *
*                                                                                                                      *
*    * Redistributions of source code must retain the above copyright notice, this list of conditions, and the         *
*      following disclaimer.                                                                                           *
*                                                                                                                      *
*    * Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the       *
*      following disclaimer in the documentation and/or other materials provided with the distribution.                *
*                                                                                                                      *
*    * Neither the name of the author nor the names of any contributors may be used to endorse or promote products     *
*      derived from this software without specific prior written permission.                                           *
*                                                                                                                      *
* THIS SOFTWARE IS PROVIDED BY THE AUTHORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED   *
* TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL *
* THE AUTHORS BE HELD LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES        *
* (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR       *
* BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT *
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE       *
* POSSIBILITY OF SUCH DAMAGE.                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@author Andrew D. Zonenberg
	@brief  Rendering code for WaveformArea
 */

#include "glscopeclient.h"
#include "WaveformArea.h"
#include "OscilloscopeWindow.h"
#include <random>
#include <map>
#include "ProfileBlock.h"
#include "../../lib/scopehal/TextRenderer.h"
#include "../../lib/scopehal/DigitalRenderer.h"
#include "../../lib/scopeprotocols/EyeDecoder2.h"
#include "../../lib/scopeprotocols/WaterfallDecoder.h"

using namespace std;
using namespace glm;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Rendering

bool WaveformArea::PrepareGeometry()
{
	//Look up some configuration and update the X axis offset
	auto pdat = dynamic_cast<AnalogCapture*>(m_channel->GetData());
	if(!pdat)
		return false;
	AnalogCapture& data = *pdat;
	m_xoff = (pdat->m_triggerPhase - m_group->m_xAxisOffset) * m_group->m_pixelsPerXUnit;
	size_t count = data.size();
	if(count == 0)
		return false;

	//Early out if nothing has changed.
	//glBufferData() and tesselation are expensive, only do them if changing LOD or new waveform data
	//if(!m_geometryDirty)
	//	return true;

	double start = GetTime();
	double xscale = pdat->m_timescale * m_group->m_pixelsPerXUnit;

	/*
	float xleft = data.GetSampleStart(j) * xscale;
	float xright = data.GetSampleStart(j+1) * xscale;
	if(xright < xleft + 1)
		xright = xleft + 1;

	float ymid = m_pixelsPerVolt * (data[j] + offset);
	float nextymid = m_pixelsPerVolt * (data[j+1] + offset);

	//Logarithmic scale for FFT displays
	if(fft)
	{
		double db1 = 20 * log10(data[j]);
		double db2 = 20 * log10(data[j+1]);

		db1 = -70 - db1;	//todo: dont hard code plot limit
		db2 = -70 - db2;

		ymid = DbToYPosition(db1);
		nextymid = DbToYPosition(db2);
	}
	*/

	//Calculate X/Y coordinate of each sample point
	//TODO: some of this can probably move to GPU too?
	m_traceBuffer.resize(count*2);
	m_indexBuffer.resize(m_width);
	m_waveformLength = count;
	double offset = m_channel->GetOffset();
	#pragma omp parallel for num_threads(8)
	for(size_t j=0; j<count; j++)
	{
		m_traceBuffer[j*2] 		= data.GetSampleStart(j) * xscale + m_xoff;
		m_traceBuffer[j*2 + 1]	= (m_pixelsPerVolt * (data[j] + offset)) + m_height/2;
	}

	double dt = GetTime() - start;
	m_prepareTime += dt;
	start = GetTime();

	//Calculate indexes for rendering.
	//This is necessary since samples may be sparse and have arbitrary spacing between them, so we can't
	//trivially map sample indexes to X pixel coordinates.
	//TODO: can we parallelize this? move to a compute shader?
	size_t nsample = 0;
	for(int j=0; j<m_width; j++)
	{
		//Default to drawing nothing
		m_indexBuffer[j] = count;

		//Move forward until we find a sample that starts in the current column
		for(; nsample < count-1; nsample ++)
		{
			//If the next sample ends after the start of the current pixel. stop
			float end = m_traceBuffer[(nsample+1)*2];
			if(end >= j)
			{
				//Start the current column at this sample
				m_indexBuffer[j] = nsample;
				break;
			}
		}
	}

	dt = GetTime() - start;
	m_indexTime += dt;
	start = GetTime();

	//Download it
	m_waveformStorageBuffer.Bind();
	glBufferData(GL_SHADER_STORAGE_BUFFER, m_traceBuffer.size()*sizeof(float), &m_traceBuffer[0], GL_STREAM_DRAW);

	//Config stuff
	uint32_t config[4];
	config[0] = m_height;							//windowHeight
	config[1] = m_plotRight;						//windowWidth
	config[2] = count;								//depth
	config[3] = m_parent->GetTraceAlpha() * 256;	//alpha
	m_waveformConfigBuffer.Bind();
	glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(config), config, GL_STREAM_DRAW);

	//Indexing
	m_waveformIndexBuffer.Bind();
	glBufferData(GL_SHADER_STORAGE_BUFFER, m_indexBuffer.size()*sizeof(uint32_t), &m_indexBuffer[0], GL_STREAM_DRAW);

	dt = GetTime() - start;
	m_downloadTime += dt;

	m_geometryDirty = false;
	return true;
}

void WaveformArea::ResetTextureFiltering()
{
	//No texture filtering
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
}

bool WaveformArea::on_render(const Glib::RefPtr<Gdk::GLContext>& /*context*/)
{
	LogIndenter li;

	double start = GetTime();
	double dt = start - m_lastFrameStart;
	if(m_lastFrameStart > 0)
	{
		//LogDebug("Inter-frame time: %.3f ms (%.2f FPS)\n", dt*1000, 1/dt);
		m_frameTime += dt;
		m_frameCount ++;
	}
	m_lastFrameStart = start;

	//Everything we draw is 2D painter's algorithm.
	//Turn off some stuff we don't need, but leave blending on.
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);

	//On the first frame, figure out what the actual screen surface FBO is.
	if(m_firstFrame)
	{
		m_windowFramebuffer.InitializeFromCurrentFramebuffer();
		m_firstFrame = false;
	}

	//Pull vertical size from the scope early on no matter how we're rendering
	m_pixelsPerVolt = m_height / m_channel->GetVoltageRange();

	//TODO: Do persistence processing

	/*
	if(!m_persistence || m_persistenceClear)
	{
		m_persistenceClear = false;
	}
	else
		RenderPersistenceOverlay();
	*/

	//Download the waveform to the GPU and kick off the compute shader for rendering it
	if(!IsEye() && !IsWaterfall())
	{
		m_geometryOK = PrepareGeometry();
		if(m_geometryOK)
			RenderTrace();
	}

	//Launch software rendering passes and push these to the GPU
	ComputeAndDownloadCairoUnderlays();
	ComputeAndDownloadCairoOverlays();

	//Actually draw the Cairo underlay
	RenderCairoUnderlays();

	//Draw the waveform stuff
	if(IsEye())
		RenderEye();
	else if(IsWaterfall())
		RenderWaterfall();
	else if(m_geometryOK)
		RenderTraceColorCorrection();

	//Draw the Cairo overlays
	RenderCairoOverlays();

	//Sanity check
	GLint err = glGetError();
	if(err != 0)
		LogNotice("Render: err = %x\n", err);

	dt = GetTime() - start;
	m_renderTime += dt;

	return true;
}

void WaveformArea::RenderEye()
{
	auto peye = dynamic_cast<EyeDecoder2*>(m_channel);
	auto pcap = dynamic_cast<EyeCapture2*>(m_channel->GetData());
	if(peye == NULL)
		return;
	if(pcap == NULL)
		return;

	//It's an eye pattern! Just copy it directly into the waveform texture.
	m_eyeTexture.Bind();
	ResetTextureFiltering();
	m_eyeTexture.SetData(
		peye->GetWidth(),
		peye->GetHeight(),
		pcap->GetData(),
		GL_RED,
		GL_FLOAT,
		GL_RGBA32F);

	//Drawing to the window
	m_windowFramebuffer.Bind(GL_FRAMEBUFFER);

	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);

	m_eyeProgram.Bind();
	m_eyeVAO.Bind();
	m_eyeProgram.SetUniform(m_eyeTexture, "fbtex", 0);
	m_eyeProgram.SetUniform(m_eyeColorRamp[m_parent->GetEyeColor()], "ramp", 1);

	//Only look at stuff inside the plot area
	glEnable(GL_SCISSOR_TEST);
	glScissor(0, 0, m_plotRight, m_height);

	glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

	glDisable(GL_SCISSOR_TEST);
	glActiveTexture(GL_TEXTURE0);
}

void WaveformArea::RenderWaterfall()
{
	auto pfall = dynamic_cast<WaterfallDecoder*>(m_channel);
	auto pcap = dynamic_cast<WaterfallCapture*>(m_channel->GetData());
	if(pfall == NULL)
		return;
	if(pcap == NULL)
		return;

	//Make sure timebase is correct
	pfall->SetTimeScale(m_group->m_pixelsPerXUnit);
	pfall->SetTimeOffset(m_group->m_xAxisOffset);

	//Just copy it directly into the waveform texture.
	m_eyeTexture.Bind();
	ResetTextureFiltering();
	m_eyeTexture.SetData(
		pfall->GetWidth(),
		pfall->GetHeight(),
		pcap->GetData(),
		GL_RED,
		GL_FLOAT,
		GL_RGBA32F);

	//Drawing to the window
	m_windowFramebuffer.Bind(GL_FRAMEBUFFER);

	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);

	m_eyeProgram.Bind();
	m_eyeVAO.Bind();
	m_eyeProgram.SetUniform(m_eyeTexture, "fbtex", 0);
	m_eyeProgram.SetUniform(m_eyeColorRamp[m_parent->GetEyeColor()], "ramp", 1);

	//Only look at stuff inside the plot area
	glEnable(GL_SCISSOR_TEST);
	glScissor(0, 0, m_plotRight, m_height);

	glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

	glDisable(GL_SCISSOR_TEST);
	glActiveTexture(GL_TEXTURE0);
}

void WaveformArea::RenderPersistenceOverlay()
{
	/*
	m_waveformFramebuffer.Bind(GL_FRAMEBUFFER);

	//Configure blending
	glEnable(GL_BLEND);
	glBlendFunc(GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA);
	glBlendColor(0, 0, 0, 0.01);
	glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);

	//Draw a black overlay with a little bit of alpha (to make old traces decay)
	m_persistProgram.Bind();
	m_persistVAO.Bind();
	glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
	*/
}

void WaveformArea::RenderTrace()
{
	//Round thread block size up to next multiple of the local size (must be power of two)
	int localSize = 2;
	int numCols = m_plotRight;
	if(0 != (numCols % localSize) )
	{
		numCols |= (localSize-1);
		numCols ++;
	}
	int numGroups = numCols / localSize;

	m_waveformComputeProgram.Bind();
	m_waveformComputeProgram.SetImageUniform(m_waveformTextureResolved, "outputTex");
	m_waveformStorageBuffer.BindBase(1);
	m_waveformConfigBuffer.BindBase(2);
	m_waveformIndexBuffer.BindBase(3);
	m_waveformComputeProgram.DispatchCompute(numGroups, 1, 1);
}

void WaveformArea::ComputeAndDownloadCairoUnderlays()
{
	double tstart = GetTime();

	//Create the Cairo surface we're drawing on
	Cairo::RefPtr< Cairo::ImageSurface > surface =
		Cairo::ImageSurface::create(Cairo::FORMAT_ARGB32, m_width, m_height);
	Cairo::RefPtr< Cairo::Context > cr = Cairo::Context::create(surface);

	//Set up transformation to match GL's bottom-left origin
	cr->translate(0, m_height);
	cr->scale(1, -1);

	//Clear to a blank background
	cr->set_source_rgba(0, 0, 0, 1);
	cr->rectangle(0, 0, m_width, m_height);
	cr->fill();

	//Software rendering
	DoRenderCairoUnderlays(cr);

	//Update the texture
	m_cairoTexture.Bind();
	ResetTextureFiltering();
	m_cairoTexture.SetData(
		m_width,
		m_height,
		surface->get_data(),
		GL_BGRA);

	m_underlayTime += (GetTime() - tstart);
}

void WaveformArea::RenderCairoUnderlays()
{
	double tstart = GetTime();

	//No blending since we're the first thing to hit the window framebuffer
	m_windowFramebuffer.Bind(GL_FRAMEBUFFER);
	glDisable(GL_BLEND);

	//Draw the actual image
	m_cairoProgram.Bind();
	m_cairoVAO.Bind();
	m_cairoProgram.SetUniform(m_cairoTexture, "fbtex");
	m_cairoTexture.Bind();
	glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

	m_underlayTime += (GetTime() - tstart);
}

void WaveformArea::DoRenderCairoUnderlays(Cairo::RefPtr< Cairo::Context > cr)
{
	RenderBackgroundGradient(cr);
	RenderGrid(cr);
}

void WaveformArea::RenderBackgroundGradient(Cairo::RefPtr< Cairo::Context > cr)
{
	//Draw the background gradient
	float ytop = m_padding;
	float ybot = m_height - 2*m_padding;
	float top_brightness = 0.1;
	float bottom_brightness = 0.0;

	Gdk::Color color(m_channel->m_displaycolor);

	Cairo::RefPtr<Cairo::LinearGradient> background_gradient = Cairo::LinearGradient::create(0, ytop, 0, ybot);
	background_gradient->add_color_stop_rgb(
		0,
		color.get_red_p() * top_brightness,
		color.get_green_p() * top_brightness,
		color.get_blue_p() * top_brightness);
	background_gradient->add_color_stop_rgb(
		1,
		color.get_red_p() * bottom_brightness,
		color.get_green_p() * bottom_brightness,
		color.get_blue_p() * bottom_brightness);
	cr->set_source(background_gradient);
	cr->rectangle(0, 0, m_plotRight, m_height);
	cr->fill();
}

int64_t WaveformArea::XPositionToXAxisUnits(float pix)
{
	return m_group->m_xAxisOffset + PixelsToXAxisUnits(pix);
}

int64_t WaveformArea::PixelsToXAxisUnits(float pix)
{
	return pix / m_group->m_pixelsPerXUnit;
}

float WaveformArea::XAxisUnitsToPixels(int64_t t)
{
	return t * m_group->m_pixelsPerXUnit;
}

float WaveformArea::XAxisUnitsToXPosition(int64_t t)
{
	return XAxisUnitsToPixels(t - m_group->m_xAxisOffset);
}

float WaveformArea::PixelsToVolts(float pix)
{
	return pix / m_pixelsPerVolt;
}

float WaveformArea::VoltsToPixels(float volt)
{
	return volt * m_pixelsPerVolt;
}

float WaveformArea::VoltsToYPosition(float volt)
{
	return m_height/2 - VoltsToPixels(volt + m_channel->GetOffset());
}

float WaveformArea::DbToYPosition(float db)
{
	float plotheight = m_height - 2*m_padding;
	return m_padding - (db/70 * plotheight);
}

float WaveformArea::YPositionToVolts(float y)
{
	return PixelsToVolts(-1 * (y - m_height/2) ) - m_channel->GetOffset();
}

void WaveformArea::RenderGrid(Cairo::RefPtr< Cairo::Context > cr)
{
	//Calculate width of right side axis label
	int twidth;
	int theight;
	Glib::RefPtr<Pango::Layout> tlayout = Pango::Layout::create (cr);
	Pango::FontDescription font("monospace normal 10");
	font.set_weight(Pango::WEIGHT_NORMAL);
	tlayout->set_font_description(font);
	tlayout->set_text("500 mV_xxx");
	tlayout->get_pixel_size(twidth, theight);
	m_plotRight = m_width - twidth;

	if(IsWaterfall())
		return;

	cr->save();

	Gdk::Color color(m_channel->m_displaycolor);

	float ytop = m_height - m_padding;
	float ybot = m_padding;
	float plotheight = m_height - 2*m_padding;
	float halfheight = plotheight/2;
	//float ymid = halfheight + ybot;

	std::map<float, float> gridmap;

	//Spectra are printed on a logarithmic scale
	if(IsFFT())
	{
		for(float db=0; db >= -60; db -= 10)
			gridmap[db] = DbToYPosition(db);
	}

	//Normal analog waveform
	else
	{
		//Volts from the center line of our graph to the top. May not be the max value in the signal.
		float volts_per_half_span = PixelsToVolts(halfheight);

		//Decide what voltage step to use. Pick from a list (in volts)
		float selected_step = AnalogRenderer::PickStepSize(volts_per_half_span);

		//Calculate grid positions
		for(float dv=0; ; dv += selected_step)
		{
			float yt = VoltsToYPosition(dv);
			float yb = VoltsToYPosition(-dv);

			if(dv != 0)
			{
				if(yb <= (ytop - theight/2) )
					gridmap[-dv] = yb;
				if(yt >= (ybot + theight/2) )
					gridmap[dv] = yt;
			}
			else
				gridmap[dv] = yt;

			//Stop if we're off the edge
			if( (yb > ytop) && (yt < ybot) )
				break;
		}

		//Center line is solid
		cr->set_source_rgba(0.7, 0.7, 0.7, 1.0);
		cr->move_to(0, VoltsToYPosition(0));
		cr->line_to(m_plotRight, VoltsToYPosition(0));
		cr->stroke();
	}

	//Dimmed lines above and below
	cr->set_source_rgba(0.7, 0.7, 0.7, 0.25);
	for(auto it : gridmap)
	{
		if(it.first == 0)	//don't over-draw the center line
			continue;
		cr->move_to(0, it.second);
		cr->line_to(m_plotRight, it.second);
	}
	cr->stroke();
	cr->unset_dash();

	//Draw background for the Y axis labels
	cr->set_source_rgba(0, 0, 0, 0.5);
	cr->rectangle(m_plotRight, 0, twidth, plotheight);
	cr->fill();

	//Draw text for the Y axis labels
	cr->set_source_rgba(1.0, 1.0, 1.0, 1.0);
	for(auto it : gridmap)
	{
		float v = it.first;

		if(IsFFT())
		{
			char tmp[32];
			snprintf(tmp, sizeof(tmp), "%.0f dB", v);
			tlayout->set_text(tmp);
		}
		else
			tlayout->set_text(m_channel->GetYAxisUnits().PrettyPrint(v));

		float y = it.second;
		if(!IsFFT())
			y -= theight/2;
		if(y < ybot)
			continue;
		if(y > ytop)
			continue;

		tlayout->get_pixel_size(twidth, theight);
		cr->move_to(m_width - twidth - 5, y);
		tlayout->update_from_cairo_context(cr);
		tlayout->show_in_cairo_context(cr);
	}
	cr->begin_new_path();

	//See if we're the active trigger
	if( (m_scope != NULL) && (m_channel->GetIndex() == m_scope->GetTriggerChannelIndex()) )
	{
		float v = m_scope->GetTriggerVoltage();
		float y = VoltsToYPosition(v);

		float trisize = 5;

		if(m_dragState == DRAG_TRIGGER)
		{
			cr->set_source_rgba(1, 0, 0, 1);
			y = m_cursorY;
		}
		else
		{
			cr->set_source_rgba(
				color.get_red_p(),
				color.get_green_p(),
				color.get_blue_p(),
				1);
		}
		cr->move_to(m_plotRight, y);
		cr->line_to(m_plotRight + trisize, y + trisize);
		cr->line_to(m_plotRight + trisize, y - trisize);
		cr->fill();
	}

	cr->restore();
}

void WaveformArea::RenderTraceColorCorrection()
{
	//Drawing to the window
	m_windowFramebuffer.Bind(GL_FRAMEBUFFER);

	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);

	Gdk::Color color(m_channel->m_displaycolor);

	m_waveformComputeProgram.MemoryBarrier();

	//Draw the offscreen buffer to the onscreen buffer
	//as a textured quad. Apply color correction as we do this.
	m_colormapProgram.Bind();
	m_colormapVAO.Bind();
	m_colormapProgram.SetUniform(m_waveformTextureResolved, "fbtex");
	m_colormapProgram.SetUniform(color.get_red_p(), "r");
	m_colormapProgram.SetUniform(color.get_green_p(), "g");
	m_colormapProgram.SetUniform(color.get_blue_p(), "b");

	glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
}

void WaveformArea::ComputeAndDownloadCairoOverlays()
{
	double tstart = GetTime();

	//Create the Cairo surface we're drawing on
	Cairo::RefPtr< Cairo::ImageSurface > surface =
		Cairo::ImageSurface::create(Cairo::FORMAT_ARGB32, m_width, m_height);
	Cairo::RefPtr< Cairo::Context > cr = Cairo::Context::create(surface);

	//Set up transformation to match GL's bottom-left origin
	cr->translate(0, m_height);
	cr->scale(1, -1);

	//Clear to a blank background
	cr->set_source_rgba(0, 0, 0, 0);
	cr->rectangle(0, 0, m_width, m_height);
	cr->set_operator(Cairo::OPERATOR_SOURCE);
	cr->fill();
	cr->set_operator(Cairo::OPERATOR_OVER);

	DoRenderCairoOverlays(cr);

	//Get the image data and make a texture from it
	m_cairoTextureOver.Bind();
	ResetTextureFiltering();
	m_cairoTextureOver.SetData(
		m_width,
		m_height,
		surface->get_data(),
		GL_BGRA);

	m_overlayTime += GetTime() - tstart;
}

void WaveformArea::RenderCairoOverlays()
{
	double tstart = GetTime();

	//Configure blending for Cairo's premultiplied alpha
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);

	//Draw the actual image
	m_windowFramebuffer.Bind(GL_FRAMEBUFFER);
	m_cairoTextureOver.Bind();
	m_cairoProgram.Bind();
	m_cairoVAO.Bind();
	m_cairoProgram.SetUniform(m_cairoTextureOver, "fbtex");
	glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

	m_overlayTime += GetTime() - tstart;
}

void WaveformArea::DoRenderCairoOverlays(Cairo::RefPtr< Cairo::Context > cr)
{
	RenderDecodeOverlays(cr);
	RenderChannelLabel(cr);
	RenderCursors(cr);
}

void WaveformArea::RenderDecodeOverlays(Cairo::RefPtr< Cairo::Context > cr)
{
	//TODO: adjust height/spacing depending on font sizes etc
	int height = 20;
	int spacing = 30;
	int midline = spacing / 2;

	//Find which overlay slots are in use
	int max_overlays = 10;
	bool overlayPositionsUsed[max_overlays] = {0};
	for(auto o : m_overlays)
	{
		if(m_overlayPositions.find(o) == m_overlayPositions.end())
			continue;

		int pos = m_overlayPositions[o];
		int index = (pos - midline) / spacing;
		if( (pos >= 0) && (index < max_overlays) )
			overlayPositionsUsed[index] = true;
	}

	//Assign first unused position to all overlays
	for(auto o : m_overlays)
	{
		if(m_overlayPositions.find(o) == m_overlayPositions.end())
		{
			for(int i=0; i<max_overlays; i++)
			{
				if(!overlayPositionsUsed[i])
				{
					overlayPositionsUsed[i] = true;
					m_overlayPositions[o] = midline + spacing*i;
					break;
				}
			}
		}
	}

	for(auto o : m_overlays)
	{
		auto render = o->CreateRenderer();
		auto data = o->GetData();

		double ymid = m_overlayPositions[o];
		double ytop = ymid - height/2;
		double ybot = ymid + height/2;

		//Render the grayed-out background
		cr->set_source_rgba(0,0,0, 0.6);
		cr->move_to(0, 				ytop);
		cr->line_to(m_plotRight, 	ytop);
		cr->line_to(m_plotRight,	ybot);
		cr->line_to(0,				ybot);
		cr->fill();

		Rect chanbox;
		RenderChannelInfoBox(o, cr, ybot, o->m_displayname, chanbox, 2);
		m_overlayBoxRects[o] = chanbox;

		int textright = chanbox.get_right() + 4;

		if(data == NULL)
			continue;

		//Handle text
		auto tr = dynamic_cast<TextRenderer*>(render);
		if(tr != NULL)
		{
			for(size_t i=0; i<data->GetDepth(); i++)
			{
				double start = (data->GetSampleStart(i) * data->m_timescale) + data->m_triggerPhase;
				double end = start + (data->GetSampleLen(i) * data->m_timescale);

				double xs = XAxisUnitsToXPosition(start);
				double xe = XAxisUnitsToXPosition(end);

				if( (xe < textright) || (xs > m_plotRight) )
					continue;

				auto text = tr->GetText(i);
				auto color = tr->GetColor(i);

				render->RenderComplexSignal(
					cr,
					textright, m_plotRight,
					xs, xe, 5,
					ybot, ymid, ytop,
					text,
					color);
			}
		}

		//Handle digital
		auto dr = dynamic_cast<DigitalRenderer*>(render);
		Gdk::Color color(o->m_displaycolor);
		cr->set_source_rgb(color.get_red_p(), color.get_green_p(), color.get_blue_p());
		bool first = true;
		double last_end = -100;
		if(dr != NULL)
		{
			auto ddat = dynamic_cast<DigitalCapture*>(data);
			for(size_t i=0; i<data->GetDepth(); i++)
			{
				double start = (data->GetSampleStart(i) * data->m_timescale) + data->m_triggerPhase;
				double end = start + (data->GetSampleLen(i) * data->m_timescale);

				double xs = XAxisUnitsToXPosition(start);
				double xe = XAxisUnitsToXPosition(end);

				if( (xe < textright) || (xs > m_plotRight) )
					continue;

				//Clamp
				if(xe > m_plotRight)
					xe = m_plotRight;

				double y = ybot;
				if((*ddat)[i])
					y = ytop;

				//Handle gaps between samples
				if( (xs - last_end) > 2)
					first = true;
				last_end = xe;

				//start of sample
				if(first)
				{
					cr->move_to(xs, y);
					first = false;
				}
				else
					cr->line_to(xs, y);

				//end of sample
				cr->line_to(xe, y);
			}
			cr->stroke();
		}

		delete render;
	}
}

void WaveformArea::RenderChannelInfoBox(
		OscilloscopeChannel* chan,
		Cairo::RefPtr< Cairo::Context > cr,
		int bottom,
		string text,
		Rect& box,
		int labelmargin)
{
	//Figure out text size
	int twidth;
	int theight;
	Glib::RefPtr<Pango::Layout> tlayout = Pango::Layout::create (cr);
	Pango::FontDescription font("sans normal 10");
	font.set_weight(Pango::WEIGHT_NORMAL);
	tlayout->set_font_description(font);
	tlayout->set_text(text);
	tlayout->get_pixel_size(twidth, theight);

	//Channel-colored rounded outline
	cr->save();

		int labelheight = theight + labelmargin*2;

		box.set_x(2);
		box.set_y(bottom - labelheight - 1);
		box.set_width(twidth + labelmargin*2);
		box.set_height(labelheight);

		Rect innerBox = box;
		innerBox.shrink(labelmargin, labelmargin);

		//Path for the outline
		cr->begin_new_sub_path();
		cr->arc(innerBox.get_left(), innerBox.get_bottom(), labelmargin, M_PI_2, M_PI);		//bottom left
		cr->line_to(box.get_left(), innerBox.get_y());
		cr->arc(innerBox.get_left(), innerBox.get_top(), labelmargin, M_PI, 1.5*M_PI);		//top left
		cr->line_to(innerBox.get_right(), box.get_top());
		cr->arc(innerBox.get_right(), innerBox.get_top(), labelmargin, 1.5*M_PI, 2*M_PI);	//top right
		cr->line_to(box.get_right(), innerBox.get_bottom());
		cr->arc(innerBox.get_right(), innerBox.get_bottom(), labelmargin, 2*M_PI, M_PI_2);	//bottom right
		cr->line_to(innerBox.get_left(), box.get_bottom());

		//Fill it
		cr->set_source_rgba(0, 0, 0, 0.75);
		cr->fill_preserve();

		//Draw the outline
		Gdk::Color color(chan->m_displaycolor);
		cr->set_source_rgba(color.get_red_p(), color.get_green_p(), color.get_blue_p(), 1);
		cr->set_line_width(1);
		cr->stroke();

	cr->restore();

	//White text
	cr->save();
		cr->set_source_rgba(1, 1, 1, 1);
		cr->move_to(labelmargin, bottom - theight - labelmargin);
		tlayout->update_from_cairo_context(cr);
		tlayout->show_in_cairo_context(cr);
	cr->restore();
}

void WaveformArea::RenderChannelLabel(Cairo::RefPtr< Cairo::Context > cr)
{
	//Add sample rate info to physical channels
	//TODO: do this to some decodes too?
	string label = m_channel->m_displayname;
	auto data = m_channel->GetData();
	if(m_channel->IsPhysicalChannel() && (data != NULL) )
	{
		label += " : ";

		//Format sample depth
		char tmp[256];
		size_t len = data->GetDepth();
		if(len > 1e6)
			snprintf(tmp, sizeof(tmp), "%.0f MS", len * 1e-6f);
		else if(len > 1e3)
			snprintf(tmp, sizeof(tmp), "%.0f kS", len * 1e-3f);
		else
			snprintf(tmp, sizeof(tmp), "%zu S", len);
		label += tmp;
		label += "\n";

		//Format timebase
		double gsps = 1000.0f / data->m_timescale;
		if(gsps > 1)
			snprintf(tmp, sizeof(tmp), "%.0f GS/s", gsps);
		else if(gsps > 0.001)
			snprintf(tmp, sizeof(tmp), "%.0f MS/s", gsps * 1000);
		else
			snprintf(tmp, sizeof(tmp), "%.1f kS/s", gsps * 1000 * 1000);
		label += tmp;
	}

	//Do the actual drawing
	RenderChannelInfoBox(m_channel, cr, m_height, label, m_infoBoxRect);
}

void WaveformArea::RenderCursors(Cairo::RefPtr< Cairo::Context > cr)
{
	int ytop = m_height;
	int ybot = 0;

	Gdk::Color yellow("yellow");
	Gdk::Color orange("orange");

	if( (m_group->m_cursorConfig == WaveformGroup::CURSOR_X_DUAL) ||
		(m_group->m_cursorConfig == WaveformGroup::CURSOR_X_SINGLE) )
	{
		//Draw first vertical cursor
		double x = XAxisUnitsToXPosition(m_group->m_xCursorPos[0]);
		cr->move_to(x, ytop);
		cr->line_to(x, ybot);
		cr->set_source_rgb(yellow.get_red_p(), yellow.get_green_p(), yellow.get_blue_p());
		cr->stroke();

		//Dual cursors
		if(m_group->m_cursorConfig == WaveformGroup::CURSOR_X_DUAL)
		{
			//Draw second vertical cursor
			double x2 = XAxisUnitsToXPosition(m_group->m_xCursorPos[1]);
			cr->move_to(x2, ytop);
			cr->line_to(x2, ybot);
			cr->set_source_rgb(orange.get_red_p(), orange.get_green_p(), orange.get_blue_p());
			cr->stroke();

			//Draw filled area between them
			cr->set_source_rgba(yellow.get_red_p(), yellow.get_green_p(), yellow.get_blue_p(), 0.2);
			cr->move_to(x, ytop);
			cr->line_to(x2, ytop);
			cr->line_to(x2, ybot);
			cr->line_to(x, ybot);
			cr->fill();
		}
	}
}
