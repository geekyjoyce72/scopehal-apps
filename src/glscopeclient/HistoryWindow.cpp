/***********************************************************************************************************************
*                                                                                                                      *
* glscopeclient                                                                                                        *
*                                                                                                                      *
* Copyright (c) 2012-2022 Andrew D. Zonenberg                                                                          *
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
	@brief  Implementation of HistoryWindow
 */
#include "glscopeclient.h"
#include "OscilloscopeWindow.h"
#include "HistoryWindow.h"
#include "FileProgressDialog.h"

using namespace std;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// HistoryColumns

HistoryColumns::HistoryColumns()
{
	add(m_timestamp);
	add(m_datestamp);
	add(m_capturekey);
	add(m_history);
	add(m_pinned);
	add(m_label);
	add(m_offset);
	add(m_marker);
	add(m_pinvisible);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Construction / destruction

HistoryWindow::HistoryWindow(OscilloscopeWindow* parent, Oscilloscope* scope)
	: Gtk::Dialog(string("History: ") + scope->m_nickname, *parent)
	, m_parent(parent)
	, m_scope(scope)
	, m_updating(false)
{
	set_skip_taskbar_hint();
	set_type_hint(Gdk::WINDOW_TYPE_HINT_DIALOG);

	set_default_size(450, 800);

	//Set up the tree view
	m_model = Gtk::TreeStore::create(m_columns);
	m_tree.set_model(m_model);
	m_tree.get_selection()->signal_changed().connect(sigc::mem_fun(*this, &HistoryWindow::OnSelectionChanged));
	m_tree.signal_button_press_event().connect_notify(sigc::mem_fun(*this, &HistoryWindow::OnTreeButtonPressEvent));
	m_model->signal_row_changed().connect(sigc::mem_fun(*this, &HistoryWindow::OnRowChanged));

	//Add the columns
	m_tree.append_column_editable("Pin", m_columns.m_pinned);
	m_tree.append_column("Date", m_columns.m_datestamp);
	m_tree.append_column("Time", m_columns.m_timestamp);
	m_tree.append_column_editable("Label", m_columns.m_label);

	//Set up visibility controls etc
	auto pincol = m_tree.get_column(0);
	auto render = m_tree.get_column_cell_renderer(0);
	pincol->add_attribute(*render, "visible", 8);

	//Set up the widgets
	get_vbox()->pack_start(m_hbox, Gtk::PACK_SHRINK);
		m_hbox.pack_start(m_maxLabel, Gtk::PACK_SHRINK);
			m_maxLabel.set_label("Max waveforms");
		m_hbox.pack_start(m_maxBox, Gtk::PACK_EXPAND_WIDGET);
			SetMaxWaveforms(10);
	get_vbox()->pack_start(m_scroller, Gtk::PACK_EXPAND_WIDGET);
		m_scroller.add(m_tree);
		m_scroller.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
			m_tree.get_selection()->set_mode(Gtk::SELECTION_BROWSE);
	get_vbox()->pack_start(m_status, Gtk::PACK_SHRINK);
		m_status.pack_end(m_memoryLabel, Gtk::PACK_SHRINK);
			m_memoryLabel.set_text("");
	show_all();

	m_contextMenu.add(m_deleteItem);
		m_deleteItem.set_label("Delete");
		m_deleteItem.signal_activate().connect(sigc::mem_fun(*this, &HistoryWindow::OnDelete));
	m_contextMenu.show_all();

	//not shown by default
	hide();

	m_lastHistoryKey.first = 0;
	m_lastHistoryKey.second = 0;
}

HistoryWindow::~HistoryWindow()
{
	//Delete old waveform data
	auto children = m_model->children();
	for(auto it : children)
	{
		WaveformHistory hist = it[m_columns.m_history];
		for(auto w : hist)
		{
			//Do *not* delete the channel's current data!
			if(w.second != w.first.m_channel->GetData(w.first.m_stream))
				delete w.second;
		}
	}
}

void HistoryWindow::SetMaxWaveforms(int n)
{
	char tmp[128];
	snprintf(tmp, sizeof(tmp), "%d", n);
	m_maxBox.set_text(tmp);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Event handlers

string HistoryWindow::FormatTimestamp(time_t base, int64_t offset)
{
	//If offset is >1 sec, shift the timestamp
	if(offset > FS_PER_SECOND)
	{
		base += (offset / FS_PER_SECOND);
		offset = offset % (int64_t)FS_PER_SECOND;
	}

	//Format timestamp
	char tmp[128];
	struct tm ltime;

#ifdef _WIN32
	localtime_s(&ltime, &base);
#else
	localtime_r(&base, &ltime);
#endif

	//round to nearest 100ps for display
	strftime(tmp, sizeof(tmp), "%X.", &ltime);
	string stime = tmp;
	snprintf(tmp, sizeof(tmp), "%010zu", static_cast<size_t>(offset / 100000));
	stime += tmp;
	return stime;
}

string HistoryWindow::FormatDate(time_t base, int64_t offset)
{
	//If offset is >1 sec, shift the timestamp
	if(offset > FS_PER_SECOND)
	{
		base += (offset / FS_PER_SECOND);
		offset = offset % (int64_t)FS_PER_SECOND;
	}

	//Format timestamp
	char tmp[128];
	struct tm ltime;

#ifdef _WIN32
	localtime_s(&ltime, &base);
#else
	localtime_r(&base, &ltime);
#endif

	//round to nearest 100ps for display
	strftime(tmp, sizeof(tmp), "%Y-%m-%d", &ltime);
	return tmp;
}

void HistoryWindow::OnWaveformDataReady(bool loading, bool pin, const string& label)
{
	//Use the timestamp from the first enabled channel
	OscilloscopeChannel* chan = NULL;
	WaveformBase* data = NULL;
	for(size_t i=0; i<m_scope->GetChannelCount(); i++)
	{
		chan = m_scope->GetChannel(i);
		if(chan->IsEnabled())
		{
			data = chan->GetData(0);
			break;
		}
	}

	//No channels at all? Nothing to do
	if( (chan == NULL) || (data == NULL) )
		return;

	//If we loaded this waveform from history, it shouldn't be put back into history again
	TimePoint key(data->m_startTimestamp, data->m_startFemtoseconds);
	if(m_lastHistoryKey == key)
		return;

	//Create the row
	m_updating = true;
	auto rowit = m_model->append();
	auto row = *rowit;
	row[m_columns.m_timestamp] = FormatTimestamp(data->m_startTimestamp, data->m_startFemtoseconds);
	row[m_columns.m_datestamp] = FormatDate(data->m_startTimestamp, data->m_startFemtoseconds);
	row[m_columns.m_capturekey] = key;
	row[m_columns.m_pinned] = pin;
	row[m_columns.m_label] = label;
	row[m_columns.m_pinvisible] = true;

	//Add waveform data
	WaveformHistory hist;
	for(size_t i=0; i<m_scope->GetChannelCount(); i++)
	{
		auto c = m_scope->GetChannel(i);
		for(size_t j=0; j<c->GetStreamCount(); j++)
		{
			auto dat = c->GetData(j);
			if(!c->IsEnabled())		//don't save historical waveforms from disabled channels
			{
				hist[StreamDescriptor(c, j)] = NULL;
				continue;
			}
			if(!dat)
				continue;
			hist[StreamDescriptor(c, j)] = dat;

			//Clear excess space out of the waveform buffer
			auto adat = dynamic_cast<AnalogWaveform*>(data);
			if(adat)
				adat->m_samples.shrink_to_fit();
		}
	}
	row[m_columns.m_history] = hist;

	//auto scroll to bottom
	auto adj = m_scroller.get_vadjustment();
	adj->set_value(adj->get_upper());

	//Select the newly added row
	m_tree.set_cursor(m_model->get_path(rowit));

	//Remove extra waveforms, if we have any.
	//When loading a file, don't delete any history even if the file has more waveforms than our current limit
	if(loading)
	{
		string smax = m_maxBox.get_text();
		size_t nmax = atoi(smax.c_str());
		auto nchildren = m_model->children().size();
		if(nmax < nchildren)
			m_maxBox.set_text(to_string(nchildren));
	}
	else
		ClearOldHistoryItems();

	UpdateMemoryUsageEstimate();

	m_updating = false;
}

void HistoryWindow::ClearOldHistoryItems()
{
	auto children = m_model->children();
	string smax = m_maxBox.get_text();
	size_t nmax = atoi(smax.c_str());

	//Clamp to 1 if the user types zero or something non-numeric
	if(nmax < 1)
	{
		m_maxBox.set_text("1");
		nmax = 1;
	}

	while(children.size() > nmax)
	{
		bool deletedSomething = false;

		//Look for the oldest un-pinned entry
		for(auto it = children.begin(); (bool)it; it++)
		{
			//Skip pinned rows
			if( (*it)[m_columns.m_pinned] )
				continue;

			//Not pinned, eligible to delete
			DeleteHistoryRow(it);
			deletedSomething = true;
			break;
		}

		//everything we could have deleted was pinned, give up
		if(!deletedSomething)
			break;
	}
}

void HistoryWindow::DeleteHistoryRow(const Gtk::TreeModel::iterator& it)
{
	//Delete any protocol analyzer state from the waveform being deleted
	auto key = (*it)[m_columns.m_capturekey];
	m_parent->RemoveProtocolHistoryFrom(key);
	m_parent->RemoveMarkersFrom(key);

	//Delete the history data
	WaveformHistory hist = (*it)[m_columns.m_history];
	for(auto w : hist)
		delete w.second;

	//and remove the row from the tree view
	m_model->erase(it);
}

void HistoryWindow::UpdateMemoryUsageEstimate()
{
	auto children = m_model->children();

	//Calculate our RAM usage (rough estimate)
	size_t bytes_used = 0;
	for(auto it : children)
	{
		WaveformHistory hist = (*it)[m_columns.m_history];
		for(auto jt : hist)
		{
			auto acap = dynamic_cast<AnalogWaveform*>(jt.second);
			if(acap != NULL)
			{
				//Add static size of the capture object
				bytes_used += sizeof(AnalogWaveform);

				//Add size of each sample
				bytes_used += sizeof(float) * acap->m_samples.capacity();
				bytes_used += sizeof(int64_t) * acap->m_offsets.capacity();
				bytes_used += sizeof(int64_t) * acap->m_durations.capacity();
			}

			auto dcap = dynamic_cast<DigitalWaveform*>(jt.second);
			if(dcap != NULL)
			{
				//Add static size of the capture object
				bytes_used += sizeof(DigitalWaveform);

				//Add size of each sample
				bytes_used += sizeof(bool) * dcap->m_samples.capacity();
				bytes_used += sizeof(int64_t) * dcap->m_offsets.capacity();
				bytes_used += sizeof(int64_t) * dcap->m_durations.capacity();
			}

			auto bcap = dynamic_cast<DigitalBusWaveform*>(jt.second);
			if(bcap != NULL)
			{
				//Add static size of the capture object
				bytes_used += sizeof(DigitalBusWaveform);

				if(!bcap->m_samples.empty())
				{
					//Add size of each sample
					bytes_used +=
						(bcap->m_samples[0].size() * sizeof(bool) + sizeof(vector<bool>))
						* bcap->m_samples.capacity();
					bytes_used += sizeof(int64_t) * bcap->m_offsets.capacity();
					bytes_used += sizeof(int64_t) * bcap->m_durations.capacity();
				}
			}
		}
	}

	//Convert to MB/GB
	char tmp[128];
	float mb = bytes_used / (1024.0f * 1024.0f);
	float gb = mb / 1024;
	if(gb > 1)
		snprintf(tmp, sizeof(tmp), "%u WFM / %.2f GB", children.size(), gb);
	else
		snprintf(tmp, sizeof(tmp), "%u WFM / %.0f MB", children.size(), mb);
	m_memoryLabel.set_label(tmp);
}

bool HistoryWindow::on_delete_event(GdkEventAny* /*ignored*/)
{
	m_parent->HideHistory();
	return true;
}

void HistoryWindow::OnSelectionChanged()
{
	//If we're updating with a new waveform we're already on the newest waveform.
	//No need to refresh anything.
	if(m_updating)
		return;

	//If we're selecting a marker etc, actually select the parent node
	auto sel = m_tree.get_selection()->get_selected();
	auto path = m_model->get_path(sel);
	bool jumpToTime = false;
	Marker* m = nullptr;
	if(path.size() > 1)
	{
		jumpToTime = true;
		m = (*sel)[m_columns.m_marker];

		path.up();
		sel = m_model->get_iter(path);
	}

	auto row = *sel;
	WaveformHistory hist = row[m_columns.m_history];
	m_lastHistoryKey = row[m_columns.m_capturekey];

	//Reload the scope with the saved waveforms
	bool actuallyChanged = false;
	for(auto it : hist)
	{
		auto chan = it.first.m_channel;
		auto stream = it.first.m_stream;
		if(chan->GetData(stream) != it.second)
		{
			actuallyChanged = true;
			chan->Detach(stream);
			chan->SetData(it.second, stream);
		}
	}

	//Tell the window to refresh everything
	if(actuallyChanged)
		m_parent->OnHistoryUpdated();

	//Move the view to the correct timestamp
	if(jumpToTime)
		m_parent->JumpToMarker(m);
}

void HistoryWindow::JumpToHistory(TimePoint timestamp)
{
	//TODO: is there a way to binary search a tree view?
	//Or get *stable* iterators that aren't invalidated by adding/removing items?
	auto children = m_model->children();
	for(auto it : children)
	{
		TimePoint key = (*it)[m_columns.m_capturekey];
		if(key == timestamp)
		{
			m_tree.get_selection()->select(it);
			break;
		}
	}
}


void HistoryWindow::ReplayHistory()
{
	//Special case if we only have one waveform
	//(select handler won't fire if we're already active)
	auto children = m_model->children();
	if(children.size() == 1)
	{
		m_parent->OnHistoryUpdated();
		m_parent->RefreshProtocolAnalyzers();
	}

	//No, iterate over everything
	else
	{
		for(auto it : children)
		{
			//Select will update all the protocol decoders etc
			m_tree.get_selection()->select(it);

			//Update analyzers
			m_parent->RefreshProtocolAnalyzers();
		}
	}
}

void HistoryWindow::OnTreeButtonPressEvent(GdkEventButton* event)
{
	if( (event->type == GDK_BUTTON_PRESS) && (event->button == 3) )
		m_contextMenu.popup(event->button, event->time);
}

void HistoryWindow::OnDelete()
{
	auto sel = m_tree.get_selection()->get_selected();
	auto path = m_model->get_path(sel);

	//It's a marker
	if(path.size() > 1)
	{
		//Select the parent node of the marker (the waveform), so we don't jump to a new marker when this one is deleted
		path.up();
		m_tree.set_cursor(path);

		//Delete the marker
		m_parent->DeleteMarker((*sel)[m_columns.m_marker]);
		m_model->erase(sel);
	}

	//It's a history row
	else
		DeleteHistoryRow(sel);
}

void HistoryWindow::OnRowChanged(const Gtk::TreeModel::Path& path, const Gtk::TreeModel::iterator& it)
{
	auto row = *it;

	//Is it a marker?
	if(path.size() > 1)
	{
		Marker* m = row[m_columns.m_marker];
		if(m == nullptr)
			return;

		//Update name
		string name = static_cast<Glib::ustring>(row[m_columns.m_label]);
		if(name != m->m_name)
		{
			m->m_name = name;
			m_parent->RefreshAllViews();
		}
	}

	//No, top level row
	else
	{
		//Any row with a label must be pinned
		if( (row[m_columns.m_label] != "") && (row[m_columns.m_pinned] != true) )
			row[m_columns.m_pinned] = true;
	}
}

void HistoryWindow::AddMarker(TimePoint stamp, int64_t offset, string name, Marker* m)
{
	//Find the node to add it under (not necessarily the current selection)
	auto children = m_model->children();
	for(auto jt : children)
	{
		auto parent = *jt;
		TimePoint pstamp = parent[m_columns.m_capturekey];
		if(pstamp != stamp)
			continue;

		//Parent node is now pinned
		parent[m_columns.m_pinned] = true;

		//Add the child item
		auto it = m_model->append(parent.children());
		auto row = *it;
		int64_t fs = stamp.second + offset;
		row[m_columns.m_capturekey] = stamp;
		row[m_columns.m_offset] = offset;
		row[m_columns.m_label] = name;
		row[m_columns.m_marker] = m;
		row[m_columns.m_pinvisible] = false;
		row[m_columns.m_datestamp] = FormatDate(stamp.first, fs);
		row[m_columns.m_timestamp] = FormatTimestamp(stamp.first, fs);

		//Make sure the row is visible
		m_tree.expand_to_path(m_model->get_path(it));

		break;
	}
}

void HistoryWindow::OnMarkerMoved(Marker* m)
{
	//TODO: faster way to find this?

	auto children = m_model->children();
	for(auto it : children)
	{
		TimePoint key = (*it)[m_columns.m_capturekey];
		if(key == m->m_point)
		{
			auto mchildren = (*it).children();
			for(auto jt : mchildren)
			{
				auto row = (*jt);
				if(row[m_columns.m_marker] == m)
				{
					int64_t fs = m->m_point.second + m->m_offset;
					row[m_columns.m_datestamp] = FormatDate(m->m_point.first, fs);
					row[m_columns.m_timestamp] = FormatTimestamp(m->m_point.first, fs);
					break;
				}
			}
			break;
		}
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Serialization

void HistoryWindow::SerializeWaveforms(
	string dir,
	IDTable& table,
	FileProgressDialog& progress,
	float base_progress,
	float progress_range)
{
	progress.Update("Saving waveform metadata", base_progress);

	//Figure out file name, and make the waveform directory
	char tmp[512];
	snprintf(tmp, sizeof(tmp), "%s/scope_%d_metadata.yml", dir.c_str(), table[m_scope]);
	string fname = tmp;
	snprintf(tmp, sizeof(tmp), "%s/scope_%d_waveforms", dir.c_str(), table[m_scope]);

#ifdef _WIN32
	mkdir(tmp);
#else
	mkdir(tmp, 0755);
#endif

	string dname = tmp;

	//Serialize waveforms
	string config = "waveforms:\n";
	auto children = m_model->children();
	int id = 1;
	size_t iwave = 0;
	float waveform_progress = progress_range / children.size();
	for(auto it : children)
	{
		auto& row = *it;

		TimePoint key = row[m_columns.m_capturekey];

		//Save metadata
		snprintf(tmp, sizeof(tmp), "    wfm%d:\n", id);
		config += tmp;
		snprintf(tmp, sizeof(tmp), "        timestamp: %ld\n", key.first);
		config += tmp;
		snprintf(tmp, sizeof(tmp), "        time_fsec: %ld\n", key.second);
		config += tmp;
		snprintf(tmp, sizeof(tmp), "        id:        %d\n", id);
		config += tmp;
		if((*it)[m_columns.m_pinned])
			config += "        pinned:    1\n";
		else
			config += "        pinned:    0\n";
		string label = str_replace("\"", "\\\"", static_cast<Glib::ustring>(row[m_columns.m_label]));
		snprintf(tmp, sizeof(tmp), "        label:     \"%s\"\n", label.c_str());
		config += tmp;
		config += "        channels:\n";

		//Format directory for this waveform
		snprintf(tmp, sizeof(tmp), "%s/waveform_%d", dname.c_str(), id);

#ifdef _WIN32
	mkdir(tmp);
#else
	mkdir(tmp, 0755);
#endif

		string wname = tmp;

		//Kick off a thread to save data for each channel
		vector<thread*> threads;
		WaveformHistory history = row[m_columns.m_history];
		size_t nchans = history.size();
		volatile float* channel_progress = new float[nchans];
		volatile int* channel_done = new int[nchans];
		size_t i=0;
		for(auto jt : history)
		{
			channel_progress[i] = 0;
			channel_done[i] = 0;

			auto chan = jt.first.m_channel;
			auto wave = jt.second;
			if((wave == NULL) || wave->m_densePacked)
			{
				threads.push_back(new thread(
					&HistoryWindow::DoSaveWaveformDataForDenseStream,
					wname,
					jt.first,
					jt.second,
					channel_progress + i,
					channel_done + i
					));
			}
			else
			{
				threads.push_back(new thread(
					&HistoryWindow::DoSaveWaveformDataForSparseStream,
					wname,
					jt.first,
					jt.second,
					channel_progress + i,
					channel_done + i
					));
			}
			i++;

			//Save channel metadata
			int index = chan->GetIndex();
			size_t nstream = jt.first.m_stream;
			if(wave == NULL)
				continue;

			snprintf(tmp, sizeof(tmp), "            ch%ds%zu:\n", index, nstream);
			config += tmp;
			if(wave->m_densePacked)
				snprintf(tmp, sizeof(tmp), "                format:       densev1\n");
			else
				snprintf(tmp, sizeof(tmp), "                format:       sparsev1\n");
			config += tmp;
			snprintf(tmp, sizeof(tmp), "                index:        %d\n", index);
			config += tmp;
			snprintf(tmp, sizeof(tmp), "                stream:       %zu\n", nstream);
			config += tmp;
			snprintf(tmp, sizeof(tmp), "                timescale:    %ld\n", wave->m_timescale);
			config += tmp;
			snprintf(tmp, sizeof(tmp), "                trigphase:    %zd\n", wave->m_triggerPhase);
			config += tmp;
		}

		//Process events and update the display with each thread's progress
		while(true)
		{
			//Figure out total progress across each channel. Stop if all threads are done
			bool done = true;
			float frac = 0;
			for(size_t j=0; j<nchans; j++)
			{
				if(!channel_done[j])
					done = false;
				frac += channel_progress[j];
			}
			if(done)
				break;
			frac /= nchans;

			//Update the UI
			snprintf(
				tmp,
				sizeof(tmp),
				"Saving waveform %zu/%zu for instrument %s: %.0f %% complete",
				iwave+1,
				(size_t)children.size(),
				m_scope->m_nickname.c_str(),
				frac * 100);
			progress.Update(tmp, base_progress + (iwave+frac)*waveform_progress);
			std::this_thread::sleep_for(std::chrono::microseconds(1000 * 50));

			g_app->DispatchPendingEvents();
		}

		delete[] channel_progress;
		delete[] channel_done;

		//Wait for threads to complete
		for(auto t : threads)
		{
			t->join();
			delete t;
		}

		id ++;
		iwave ++;
	}

	//Save waveform metadata
	FILE* fp = fopen(fname.c_str(), "w");
	if(!fp)
	{
		string msg = string("The data file ") + fname + " could not be created!";
		Gtk::MessageDialog errdlg(msg, false, Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK, true);
		errdlg.set_title("Cannot save session\n");
		errdlg.run();
		return;
	}
	if(config.length() != fwrite(config.c_str(), 1, config.length(), fp))
	{
		string msg = string("Error writing to session file ") + fname + "!";
		Gtk::MessageDialog errdlg(msg, false, Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK, true);
		errdlg.set_title("Cannot save session\n");
		errdlg.run();
	}
	fclose(fp);
}

/**
	@brief Saves waveform sample data in the "sparsev1" file format.

	Interleaved (slow):
		int64 offset
		int64 len
		for analog
			float voltage
		for digital
			bool voltage
 */
void HistoryWindow::DoSaveWaveformDataForSparseStream(
	std::string wname,
	StreamDescriptor stream,
	WaveformBase* wave,
	volatile float* progress,
	volatile int* done
	)
{
	auto chan = stream.m_channel;
	int index = chan->GetIndex();
	size_t nstream = stream.m_stream;
	if(wave == NULL)		//trigger, disabled, etc
	{
		*done = 1;
		*progress = 1;
		return;
	}

	//First stream has no suffix for compat
	char tmp[512];
	if(nstream == 0)
		snprintf(tmp, sizeof(tmp), "%s/channel_%d.bin", wname.c_str(), index);
	else
		snprintf(tmp, sizeof(tmp), "%s/channel_%d_stream%zu.bin", wname.c_str(), index, nstream);

	FILE* fp = fopen(tmp, "wb");

	auto achan = dynamic_cast<AnalogWaveform*>(wave);
	auto dchan = dynamic_cast<DigitalWaveform*>(wave);
	size_t len = wave->m_offsets.size();

	//Analog channels
	const size_t samples_per_block = 10000;
	if(achan)
	{
		#pragma pack(push, 1)
		class asample_t
		{
		public:
			int64_t off;
			int64_t dur;
			float voltage;

			asample_t(int64_t o=0, int64_t d=0, float v=0)
			: off(o), dur(d), voltage(v)
			{}
		};
		#pragma pack(pop)

		//Copy sample data
		vector<asample_t,	AlignedAllocator<asample_t, 64 > > samples;
		samples.reserve(len);
		for(size_t i=0; i<len; i++)
			samples.push_back(asample_t(wave->m_offsets[i], wave->m_durations[i], achan->m_samples[i]));

		//Write it
		for(size_t i=0; i<len; i+= samples_per_block)
		{
			*progress = i * 1.0 / len;
			size_t blocklen = min(len-i, samples_per_block);

			if(blocklen != fwrite(&samples[i], sizeof(asample_t), blocklen, fp))
				LogError("file write error\n");
		}
	}
	else if(dchan)
	{
		#pragma pack(push, 1)
		class dsample_t
		{
		public:
			int64_t off;
			int64_t dur;
			bool voltage;

			dsample_t(int64_t o=0, int64_t d=0, bool v=0)
			: off(o), dur(d), voltage(v)
			{}
		};
		#pragma pack(pop)

		//Copy sample data
		vector<dsample_t,	AlignedAllocator<dsample_t, 64 > > samples;
		samples.reserve(len);
		for(size_t i=0; i<len; i++)
			samples.push_back(dsample_t(wave->m_offsets[i], wave->m_durations[i], dchan->m_samples[i]));

		//Write it
		for(size_t i=0; i<len; i+= samples_per_block)
		{
			*progress = i * 1.0 / len;
			size_t blocklen = min(len-i, samples_per_block);

			if(blocklen != fwrite(&samples[i], sizeof(dsample_t), blocklen, fp))
				LogError("file write error\n");
		}
	}
	else
	{
		//TODO: support other waveform types (buses, eyes, etc)
		LogError("unrecognized sample type\n");
	}

	fclose(fp);

	*done = 1;
	*progress = 1;
}

/**
	@brief Saves waveform sample data in the "densev1" file format.

	for analog
		float[] voltage
	for digital
		bool[] voltage

	Durations are implied {1....1} and offsets are implied {0...n-1}.
 */
void HistoryWindow::DoSaveWaveformDataForDenseStream(
	std::string wname,
	StreamDescriptor stream,
	WaveformBase* wave,
	volatile float* progress,
	volatile int* done
	)
{
	auto chan = stream.m_channel;
	int index = chan->GetIndex();
	size_t nstream = stream.m_stream;
	if(wave == NULL)		//trigger, disabled, etc
	{
		*done = 1;
		*progress = 1;
		return;
	}

	//First stream has no suffix for compat
	char tmp[512];
	if(nstream == 0)
		snprintf(tmp, sizeof(tmp), "%s/channel_%d.bin", wname.c_str(), index);
	else
		snprintf(tmp, sizeof(tmp), "%s/channel_%d_stream%zu.bin", wname.c_str(), index, nstream);

	FILE* fp = fopen(tmp, "wb");

	auto achan = dynamic_cast<AnalogWaveform*>(wave);
	auto dchan = dynamic_cast<DigitalWaveform*>(wave);
	size_t len = wave->m_offsets.size();

	//Analog channels
	const size_t samples_per_block = 10000;
	if(achan)
	{
		//Write it
		for(size_t i=0; i<len; i+= samples_per_block)
		{
			*progress = i * 1.0 / len;
			size_t blocklen = min(len-i, samples_per_block);

			if(blocklen != fwrite(&achan->m_samples[i], sizeof(float), blocklen, fp))
				LogError("file write error\n");
		}
	}
	else if(dchan)
	{
		//Write it
		for(size_t i=0; i<len; i+= samples_per_block)
		{
			*progress = i * 1.0 / len;
			size_t blocklen = min(len-i, samples_per_block);

			if(blocklen != fwrite(&dchan->m_samples[i], sizeof(bool), blocklen, fp))
				LogError("file write error\n");
		}
	}
	else
	{
		//TODO: support other waveform types (buses, eyes, etc)
		LogError("unrecognized sample type\n");
	}

	fclose(fp);

	*done = 1;
	*progress = 1;
}
