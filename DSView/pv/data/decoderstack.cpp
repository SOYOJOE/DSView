/*
 * This file is part of the PulseView project.
 * DSView is based on PulseView.
 * 
 * Copyright (C) 2012 Joel Holdsworth <joel@airwebreathe.org.uk>
 * Copyright (C) 2014 DreamSourceLab <support@dreamsourcelab.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */
  

#include <stdexcept>
#include <algorithm>
#include <assert.h>
#include <chrono>
#include <cmath>
#include <cstring>

#include "decoderstack.h"
#include "logicsnapshot.h"
#include "decode/decoder.h"
#include "decode/annotation.h"
#include "decode/rowdata.h"
#include "../sigsession.h"
#include "../view/logicsignal.h"
#include "../dsvdef.h"
#include "../log.h"
#include "../ui/langresource.h"
#include <ds_types.h>

using namespace pv::data::decode;
using namespace std;
using namespace boost;

namespace pv {
namespace data {

static std::mutex srd_session_mutex;

static int variant_to_int(GVariant *value, int default_value)
{
    if (value == NULL)
        return default_value;

    const GVariantClass klass = g_variant_classify(value);
    switch (klass) {
    case G_VARIANT_CLASS_BYTE:
        return g_variant_get_byte(value);
    case G_VARIANT_CLASS_INT16:
        return g_variant_get_int16(value);
    case G_VARIANT_CLASS_UINT16:
        return g_variant_get_uint16(value);
    case G_VARIANT_CLASS_INT32:
        return g_variant_get_int32(value);
    case G_VARIANT_CLASS_UINT32:
        return g_variant_get_uint32(value);
    case G_VARIANT_CLASS_INT64:
        return (int)g_variant_get_int64(value);
    case G_VARIANT_CLASS_UINT64:
        return (int)g_variant_get_uint64(value);
    case G_VARIANT_CLASS_DOUBLE:
        return (int)g_variant_get_double(value);
    default:
        return default_value;
    }
}

static double variant_to_double(GVariant *value, double default_value)
{
    if (value == NULL)
        return default_value;

    const GVariantClass klass = g_variant_classify(value);
    switch (klass) {
    case G_VARIANT_CLASS_DOUBLE:
        return g_variant_get_double(value);
    case G_VARIANT_CLASS_INT32:
        return g_variant_get_int32(value);
    case G_VARIANT_CLASS_UINT32:
        return g_variant_get_uint32(value);
    case G_VARIANT_CLASS_INT64:
        return (double)g_variant_get_int64(value);
    case G_VARIANT_CLASS_UINT64:
        return (double)g_variant_get_uint64(value);
    default:
        return default_value;
    }
}

static const char *variant_to_string(GVariant *value, const char *default_value)
{
    if (value == NULL || !g_variant_is_of_type(value, G_VARIANT_TYPE_STRING))
        return default_value;
    return g_variant_get_string(value, NULL);
}

static QString native_uart_text(uint32_t value, int data_bits, const char *format)
{
    if (strcmp(format, "ascii") == 0) {
        if (value >= 0x20 && value <= 0x7e)
            return QString(QChar((char)value));
        if (value == '\r')
            return QString("\\r");
        if (value == '\n')
            return QString("\\n");
        if (value == '\t')
            return QString("\\t");
        if (value == 0)
            return QString("\\0");
        return QString("\\x%1").arg(value, 2, 16, QChar('0')).toUpper();
    }
    if (strcmp(format, "dec") == 0)
        return QString::number(value);
    if (strcmp(format, "oct") == 0)
        return QString::number(value, 8);
    if (strcmp(format, "bin") == 0)
        return QString::number(value, 2).rightJustified(data_bits, QChar('0'));

    return QString("@%1").arg(value, 2, 16, QChar('0')).toUpper();
}

const double DecoderStack::DecodeMargin = 1.0;
const double DecoderStack::DecodeThreshold = 0.2;
const int64_t DecoderStack::DecodeChunkLength = 4 * 1024; 
const unsigned int DecoderStack::DecodeNotifyPeriod = 1024;
 
DecoderStack::DecoderStack(pv::SigSession *session,
	const srd_decoder *const dec, DecoderStatus *decoder_status) :
	_session(session)
{
    assert(session);
    assert(dec);
    assert(decoder_status); 
    
    _samples_decoded = 0;
    _sample_count = 0; 
    _decode_state = Stopped;
    _options_changed = false;
    _no_memory = false;
    _mark_index = -1;
    _decoder_status = decoder_status;
    _stask_stauts = NULL; 
    _is_capture_end = true;
    _snapshot = NULL;
    _progress = 0;
    _is_decoding = false;
    _result_count = 0;
    
    _stack.push_back(new decode::Decoder(dec));
 
    build_row();
}

DecoderStack::~DecoderStack()
{   
    //release resource talbe
    DESTROY_OBJECT(_decoder_status);

    //release source
    for (auto &kv : _rows)
    {
        kv.second->clear(); //destory all annotations
        delete kv.second;
    }
    _rows.clear();

    //Decoder
    for (auto *p : _stack){
        delete p;
    }
    _stack.clear();
    
    _rows_gshow.clear();
    _rows_lshow.clear();
    _class_rows.clear();
}
 
void DecoderStack::add_sub_decoder(decode::Decoder *decoder)
{
	assert(decoder);
	_stack.push_back(decoder);
    build_row();
    _options_changed = true;
}

void DecoderStack::remove_sub_decoder(Decoder *decoder)
{
	// Find the decoder in the stack
    auto  iter = _stack.begin();
    for(unsigned int i = 0; i < _stack.size(); i++, iter++)
        if ((*iter) == decoder)
            break;

	// Delete the element
    if (iter != _stack.end())
    {
        _stack.erase(iter);
        delete decoder;
    }        

    build_row();
    _options_changed = true;
}

void DecoderStack::remove_decoder_by_handel(const srd_decoder *dec)
{
    Decoder *decoder = NULL;

    for (auto d : _stack){
        if (d->get_dec_handel() == dec){
            decoder = d;
            break;
        }
    }

    if (decoder){
        remove_sub_decoder(decoder);
    }
}

void DecoderStack::build_row()
{
    //release source
    for (auto &kv : _rows)
    {   
        kv.second->clear(); //destory all annotations
        delete kv.second;
    }
    _rows.clear();

    // Add classes
    for (auto dec : _stack)
    { 
        const srd_decoder *const decc = dec->decoder();
        assert(dec->decoder());

        dec->reset_start();

        // Add a row for the decoder if it doesn't have a row list
        if (!decc->annotation_rows) {
            const Row row(decc);
            _rows[row] = new decode::RowData();
            std::map<const decode::Row, bool>::const_iterator iter = _rows_gshow.find(row);
            if (iter == _rows_gshow.end()) {
                _rows_gshow[row] = true;
                if (row.title().contains("bit", Qt::CaseInsensitive) ||
                    row.title().contains("warning", Qt::CaseInsensitive)) {
                    _rows_lshow[row] = false;
                } else {
                    _rows_lshow[row] = true;
                }
            }
        }

        // Add the decoder rows
        int order = 0;
        for (const GSList *l = decc->annotation_rows; l; l = l->next)
        {
            const srd_decoder_annotation_row *const ann_row =
                (srd_decoder_annotation_row *)l->data;
            assert(ann_row);

            const Row row(decc, ann_row, order);

            // Add a new empty row data object
            _rows[row] = new decode::RowData();
            std::map<const decode::Row, bool>::const_iterator iter = _rows_gshow.find(row);
            if (iter == _rows_gshow.end()) {
                _rows_gshow[row] = true;
                if (row.title().contains("bit", Qt::CaseInsensitive) ||
                    row.title().contains("warning", Qt::CaseInsensitive)) {
                    _rows_lshow[row] = false;
                } else {
                    _rows_lshow[row] = true;
                }
            }

            // Map out all the classes
            for (const GSList *ll = ann_row->ann_classes; ll; ll = ll->next){
                _class_rows[make_pair(decc, GPOINTER_TO_INT(ll->data))] = Row(row);
            }

            order++;
        }
    }
}

int64_t DecoderStack::samples_decoded()
{
    std::lock_guard<std::mutex> decode_lock(_output_mutex);
	return _samples_decoded;
}

void DecoderStack::get_annotation_subset(
	std::vector<pv::data::decode::Annotation*> &dest,
	const Row &row, uint64_t start_sample,
	uint64_t end_sample)
{  
    auto iter = _rows.find(row);
    if (iter != _rows.end())
        (*iter).second->get_annotation_subset(dest,
			start_sample, end_sample);
}


uint64_t DecoderStack::get_annotation_index(
    const Row &row, uint64_t start_sample)
{  
    uint64_t index = 0;
    auto iter = _rows.find(row);
    if (iter != _rows.end())
        index = (*iter).second->get_annotation_index(start_sample);

    return index;
}

uint64_t DecoderStack::get_max_annotation(const Row &row)
{ 
    auto iter =  _rows.find(row);
    if (iter != _rows.end())
        return (*iter).second->get_max_annotation();

    return 0;
}

uint64_t DecoderStack::get_min_annotation(const Row &row)
{  
    auto iter = _rows.find(row);
    if (iter != _rows.end())
        return (*iter).second->get_min_annotation();

    return 0;
}

std::map<const decode::Row, bool> DecoderStack::get_rows_gshow()
{
    std::map<const decode::Row, bool> rows_gshow;
    for (std::map<const decode::Row, bool>::const_iterator i = _rows_gshow.begin();
        i != _rows_gshow.end(); i++) {
        rows_gshow[(*i).first] = (*i).second;
    }
    return rows_gshow;
}

std::map<const decode::Row, bool> DecoderStack::get_rows_lshow()
{
    std::map<const decode::Row, bool> rows_lshow;
    for (std::map<const decode::Row, bool>::const_iterator i = _rows_lshow.begin();
        i != _rows_lshow.end(); i++) {
        rows_lshow[(*i).first] = (*i).second;
    }
    return rows_lshow;
}

void DecoderStack::set_rows_gshow(const decode::Row row, bool show)
{
    std::map<const decode::Row, bool>::const_iterator iter = _rows_gshow.find(row);
    if (iter != _rows_gshow.end()) {
        _rows_gshow[row] = show;
    }
}

void DecoderStack::set_rows_lshow(const decode::Row row, bool show)
{
    std::map<const decode::Row, bool>::const_iterator iter = _rows_lshow.find(row);
    if (iter != _rows_lshow.end()) {
        _rows_lshow[row] = show;
    }
}

bool DecoderStack::has_annotations(const Row &row)
{  
    auto iter =
        _rows.find(row);
    if (iter != _rows.end())
        if(0 == (*iter).second->get_max_sample())
            return false;
        else
            return true;
    else
        return false;
}

uint64_t DecoderStack::list_annotation_size()
{
    std::lock_guard<std::mutex> lock(_output_mutex);
    uint64_t max_annotation_size = 0;

    for (auto it = _rows.begin(); it != _rows.end(); it++) {
        auto iter = _rows_lshow.find((*it).first);
        if (iter != _rows_lshow.end() && (*iter).second){
            max_annotation_size = max(max_annotation_size,
                (*it).second->get_annotation_size());
        }
    }

    return max_annotation_size;
}

uint64_t DecoderStack::list_annotation_size(uint16_t row_index)
{ 
    for (auto i = _rows.begin(); i != _rows.end(); i++) {
        auto iter = _rows_lshow.find((*i).first);
        if (iter != _rows_lshow.end() && (*iter).second)
            if (row_index-- == 0) {
                return (*i).second->get_annotation_size();
            }
    }
    return 0;
}

bool DecoderStack::list_annotation(pv::data::decode::Annotation *ann,
                                  uint16_t row_index, uint64_t col_index)
{ 
    for (auto i = _rows.begin(); i != _rows.end(); i++) {
        auto iter = _rows_lshow.find((*i).first);
        if (iter != _rows_lshow.end() && (*iter).second) {
            if (row_index-- == 0) {
                return (*i).second->get_annotation(ann, col_index);
            }
        }
    }

    return false;
}


bool DecoderStack::list_row_title(int row, QString &title)
{ 
    for (auto i = _rows.begin();i != _rows.end(); i++) {
        auto iter = _rows_lshow.find((*i).first);
        if (iter != _rows_lshow.end() && (*iter).second) {
            if (row-- == 0) {
                title = (*i).first.title();
                return 1;
            }
        }
    }
    return 0;
}

void DecoderStack::clear()
{
    init();
}

void DecoderStack::init()
{
    _sample_count = 0; 
    _samples_decoded = 0;
    _error_message = QString();
    _no_memory = false;
    _snapshot = NULL;
    _result_count = 0;

    for (auto i = _rows.begin();i != _rows.end(); i++) { 
        (*i).second->clear();
    }

    set_mark_index(-1);
}
 
void DecoderStack::stop_decode_work()
{  
    //set the flag to exit from task thread 
     if (_stask_stauts){
         _stask_stauts->_bStop = true;
     }
    _decode_state = Stopped; 
}

void DecoderStack::begin_decode_work()
{
     assert(_decode_state == Stopped);

     _error_message = "";
     _decode_state = Running;
      do_decode_work();
     _decode_state = Stopped;
}

bool DecoderStack::check_required_probes()
{
    for(auto dec : _stack){
		if (!dec->have_required_probes()) {
			return false;
		}
    }

    return true;
}

void DecoderStack::do_decode_work()
{
    //set the flag to exit from task thread 
     if (_stask_stauts){
         _stask_stauts->_bStop = true;
     }
     _stask_stauts = new decode_task_status();
     _stask_stauts->_bStop = false;
     _stask_stauts->_decoder = this;
     _decoder_status->clear(); //clear old items

    if (!_options_changed)
    {  
        dsv_err("ERROR:Decoder options have not changed.");
        return;
    } 
    _options_changed = false;

    init();

    _snapshot = NULL;

	// Check that all decoders have the required channels
    if (!check_required_probes()) {
        _error_message = L_S(STR_PAGE_MSG, S_ID(IDS_MSG_DECODERSTACK_DECODE_WORK_ERROR),
                            "One or more required channels have not been specified");
        dsv_err("ERROR:%s", _error_message.toStdString().c_str());
        return;
	}

	// We get the logic data of the first channel in the list.
	// This works because we are currently assuming all
	// LogicSignals have the same data/snapshot
    for (auto dec : _stack) {
        if (dec->have_probes()) {
            for(auto s :  _session->get_signals()) {
                if(s->get_index() == dec->first_probe_index() && s->signal_type() == SR_CHANNEL_LOGIC)
                { 
                    _snapshot = ((pv::view::LogicSignal*)s)->data();
                    if (_snapshot != NULL)
                        break;
                }
            }
            if (_snapshot != NULL)
                break;
        }
    }

	if (_snapshot == NULL)
    {   
        _error_message = L_S(STR_PAGE_MSG, S_ID(IDS_MSG_DECODERSTACK_DECODE_WORK_ERROR),
                             "One or more required channels have not been specified");
        dsv_err("ERROR:%s", _error_message.toStdString().c_str());
        return;
    }		

    if (_session->is_realtime_refresh() == false && _snapshot->empty())
    { 
        dsv_err("ERROR:Decode data is empty.");
        return;
    }

    // Get the samplerate
	_samplerate = _snapshot->samplerate();
    if (_samplerate == 0.0)
    {
        dsv_err("ERROR:Decode data got an invalid sample rate.");
        return;
    }
     
    dsv_info("DecoderStack::do_decode_work root=%s stack=%u options_changed=1",
             get_root_decoder_id() ? get_root_decoder_id() : "NULL",
             (unsigned int)_stack.size());

    if (!execute_native_uart_decode())
        execute_decode_stack();
}

uint64_t DecoderStack::get_max_sample_count()
{
	uint64_t max_sample_count = 0;

    for (auto i = _rows.begin(); i != _rows.end(); i++){
        max_sample_count = max(max_sample_count, (*i).second->get_max_sample());
    } 	

	return max_sample_count;
}

void DecoderStack::decode_data(const uint64_t decode_start, const uint64_t decode_end, srd_session *const session)
{
    decode_task_status *status = _stask_stauts;

    //uint8_t *chunk = NULL;
    uint64_t last_cnt = 0;
    uint64_t notify_cnt = (decode_end - decode_start + 1)/100;
    srd_decoder_inst *logic_di = NULL;

    // find the first level decoder instant
    for (GSList *d = session->di_list; d; d = d->next) {
        srd_decoder_inst *di = (srd_decoder_inst *)d->data;
        srd_decoder *decoder = di->decoder;
        const bool have_probes = (decoder->channels || decoder->opt_channels) != 0;
        if (have_probes) {
            logic_di = di;
            break;
        }
    }

    assert(logic_di);

    uint64_t entry_cnt = 0;
    uint64_t i = decode_start;
    char *error = NULL; 
    bool bError = false;
    bool bEndTime = false;
    //struct srd_push_param push_param;

    if( i >= decode_end){
        dsv_info("decode data index have been to end");
    }

    std::vector<const uint8_t *> chunk;
    std::vector<uint8_t> chunk_const;

    bool bCheckEnd = false;
    uint64_t end_index = decode_end;

    _progress = 0;
    uint64_t sended_len  = 0;
    _is_decoding = true;

    void* lbp_array[35];

    for (int j =0 ; j < logic_di->dec_num_channels; j++){
        lbp_array[j] = NULL;
    }
  
    while(i < end_index && !_no_memory && !status->_bStop)
    {
        chunk.clear();
        chunk_const.clear();

        if (_is_capture_end)
        {
            if (!bCheckEnd){
                bCheckEnd = true;

                uint64_t align_sample_count = _snapshot->get_ring_sample_count();

                if (end_index >= align_sample_count){
                    end_index = align_sample_count - 1;
                    dsv_info("Reset the decode end sample, new:%llu, old:%llu", 
                        (u64_t)end_index, (u64_t)decode_end);
                }

                if (i >= align_sample_count){
                    dsv_info("ERROR: the decoding sample index is out of range.");
                    break;
                }
            }
        }
        else if (i >= _snapshot->get_ring_sample_count())
        {   
            // Wait the data is ready.
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        if (_is_capture_end && i == _snapshot->get_ring_sample_count()){
            break;
        }

        uint64_t chunk_end = end_index;
        if (!_snapshot->is_sparse() && chunk_end - i > MaxChunkSize)
            chunk_end = i + MaxChunkSize;

        for (int j =0 ; j < logic_di->dec_num_channels; j++) {
            int sig_index = logic_di->dec_channelmap[j];
            void *lbp = NULL;

            if (sig_index == -1) {
                chunk.push_back(NULL);
                chunk_const.push_back(0);
            }
            else {
                if (_snapshot->has_data(sig_index)) {
                    const uint8_t *data_ptr = _snapshot->get_samples(i, chunk_end, sig_index, &lbp);
                    chunk.push_back(data_ptr);
                    chunk_const.push_back(_snapshot->get_sample(i, sig_index));

                    if (_snapshot->is_able_free() == false)
                    {
                        if (lbp_array[j] != lbp){
                            if (lbp_array[j] != NULL)
                                _snapshot->free_decode_lpb(lbp_array[j]);
                            lbp_array[j] = lbp;
                        }
                    }
                }
                else {
                    _error_message = L_S(STR_PAGE_MSG, S_ID(IDS_MSG_DECODERSTACK_DECODE_DATA_ERROR),
                                     "At least one of selected channels are not enabled.");
                    return;
                }
            }
        }

        if (chunk_end > end_index)
            chunk_end = end_index;

        bEndTime = (chunk_end == end_index);

        if (srd_session_send(
                session,
                i,
                chunk_end,
                chunk.data(),
                chunk_const.data(),
                chunk_end - i,
                &error) != SRD_OK){

            if (error){
                _error_message = QString::fromLocal8Bit(error);
                dsv_err("Failed to call srd_session_send:%s", error);
                g_free(error);
                error = NULL;
            }

            bError = true;
            break;
        }

        sended_len += chunk_end - i; 
        _progress = (int)(sended_len * 100 / end_index);

        i = chunk_end;       

        //use mutex
        {
            std::lock_guard<std::mutex> lock(_output_mutex);
            _samples_decoded = i - decode_start + 1;
        }

        if ((i - last_cnt) > notify_cnt) {
            last_cnt = i;
            new_decode_data();
        }

        entry_cnt++;
    }

    _progress = 100;
    _is_decoding = false;
    
    new_decode_data();

    // the task is normal ends,so all samples was processed;
    if (!bError && bEndTime){
       srd_session_end(session, &error);

        if (error != NULL){
            _error_message = QString::fromLocal8Bit(error);
            dsv_err("Failed to call srd_session_end:%s", error);
        }
    }
 
    dsv_info("%s%llu", "send to decoder times: ", (u64_t)entry_cnt);

    if (error != NULL)
        g_free(error);
  
    if (!_session->is_closed())
        decode_done();
}

bool DecoderStack::push_native_annotation(uint64_t start_sample, uint64_t end_sample,
                                          int format, int type,
                                          const std::vector<QString> &annotations)
{
    Annotation *a = new Annotation(start_sample, end_sample, format, type,
                                   annotations, _decoder_status);
    if (a == NULL) {
        _no_memory = true;
        return false;
    }
    _result_count++;

    const srd_decoder *const decc = _stack.front()->decoder();
    auto row_iter = _rows.end();
    const auto r = _class_rows.find(make_pair(decc, a->format()));
    if (r != _class_rows.end())
        row_iter = _rows.find((*r).second);
    else
        row_iter = _rows.find(Row(decc));

    if (row_iter == _rows.end()) {
        delete a;
        dsv_err("Unexpected native UART annotation: decoder = %p, format = %d",
                (void*)decc, format);
        return false;
    }

    if (!(*row_iter).second->push_annotation(a))
        _no_memory = true;

    return !_no_memory;
}

bool DecoderStack::execute_native_uart_decode()
{
    if (_stack.size() != 1) {
        dsv_info("native uart skip: stack size=%u", (unsigned int)_stack.size());
        return false;
    }

    decode::Decoder *dec = _stack.front();
    const srd_decoder *const decc = dec->decoder();
    if (decc == NULL || decc->id == NULL || strcmp(decc->id, "0:uart") != 0) {
        dsv_info("native uart skip: decoder id=%s",
                 (decc && decc->id) ? decc->id : "NULL");
        return false;
    }

    if (_session->is_loop_mode() && !_is_capture_end) {
        dsv_info("native uart defer: loop capture is running");
        return true;
    }

    const int sig_index = dec->first_probe_index();
    decode_task_status *status = _stask_stauts;
    while (!_is_capture_end && !status->_bStop &&
           (sig_index < 0 || !_snapshot->has_data(sig_index))) {
        dsv_info("native uart wait data: channel=%d enabled=[%s]",
                 sig_index, _snapshot->enabled_channel_text().toUtf8().constData());
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (sig_index < 0 || !_snapshot->has_data(sig_index)) {
        dsv_info("native uart skip: invalid channel=%d has_data=%d enabled=[%s]",
                 sig_index, sig_index >= 0 ? (_snapshot->has_data(sig_index) ? 1 : 0) : 0,
                 _snapshot->enabled_channel_text().toUtf8().constData());
        return false;
    }

    uint64_t decode_start = dec->decode_start();
    uint64_t decode_end = 0;
    _sample_count = _snapshot->get_ring_sample_count();
    if (_sample_count == 0) {
        dsv_info("native uart skip: sample_count=0");
        return true;
    }

    decode_end = min(dec->decode_end(), _sample_count - 1);

    if (decode_start >= decode_end) {
        dsv_info("native uart skip: empty region start=%llu end=%llu sample_count=%llu",
                 (u64_t)decode_start, (u64_t)decode_end, (u64_t)_sample_count);
        return true;
    }

    auto &options = dec->options();
    const int baudrate = variant_to_int(options["baudrate"], 115200);
    const int data_bits = variant_to_int(options["num_data_bits"], 8);
    const double stop_bits = variant_to_double(options["num_stop_bits"], 1.0);
    const bool invert = strcmp(variant_to_string(options["invert"], "no"), "yes") == 0;
    const bool msb_first = strcmp(variant_to_string(options["bit_order"], "lsb-first"), "msb-first") == 0;
    const bool show_startstop = strcmp(variant_to_string(options["anno_startstop"], "no"), "yes") == 0;
    const char *display_format = variant_to_string(options["format"], "hex");
    const char *parity_type = variant_to_string(options["parity_type"], "none");
    const bool has_parity = strcmp(parity_type, "none") != 0;

    if (baudrate <= 0 || data_bits <= 0 || data_bits > 32) {
        dsv_info("native uart skip: bad options baud=%d data_bits=%d",
                 baudrate, data_bits);
        return false;
    }

    const double bit_width = (double)_samplerate / (double)baudrate;
    if (bit_width < 1.0) {
        dsv_info("native uart skip: bit_width=%f samplerate=%llu baud=%d",
                 bit_width, (u64_t)_samplerate, baudrate);
        return false;
    }

    const uint64_t requested_decode_end = dec->decode_end();
    const uint64_t notify_step = max<uint64_t>((decode_end - decode_start + 1) / 20, 1);
    uint64_t last_notify = decode_start;
    uint64_t index = decode_start;
    const bool use_unlocked_snapshot = _is_capture_end && _snapshot->get_loop_offset() == 0;
    const auto native_start_time = std::chrono::steady_clock::now();
    dsv_info("native uart decode: ch=%d, start=%llu, end=%llu, baud=%d, unlocked=%d",
             sig_index, (u64_t)decode_start, (u64_t)decode_end, baudrate,
             use_unlocked_snapshot ? 1 : 0);
    auto get_sample = [&](uint64_t sample) -> bool {
        return use_unlocked_snapshot ?
            _snapshot->get_sample_no_lock(sample, sig_index) :
            _snapshot->get_sample(sample, sig_index);
    };
    auto get_nxt_edge = [&](uint64_t &sample, bool last_sample) -> bool {
        return use_unlocked_snapshot ?
            _snapshot->get_nxt_edge_no_lock(sample, last_sample, decode_end, 0, sig_index) :
            _snapshot->get_nxt_edge(sample, last_sample, decode_end, 0, sig_index);
    };

    bool last_raw = get_sample(index);
    index++;

    _progress = 0;
    _is_decoding = true;

    while (!_no_memory && !status->_bStop) {
        _sample_count = _snapshot->get_ring_sample_count();
        if (_sample_count == 0) {
            if (_is_capture_end)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        decode_end = min(requested_decode_end, _sample_count - 1);
        if (index >= decode_end) {
            if (_is_capture_end)
                break;
            if (decode_end > decode_start)
                last_raw = get_sample(decode_end);
            index = decode_end;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        uint64_t edge = index;
        if (!get_nxt_edge(edge, last_raw)) {
            if (_is_capture_end)
                break;
            if (decode_end > decode_start)
                last_raw = get_sample(decode_end);
            index = decode_end;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        const bool prev_raw = last_raw;
        const bool cur_raw = get_sample(edge);
        const bool prev_level = invert ? !last_raw : last_raw;
        const bool cur_level = invert ? !cur_raw : cur_raw;
        last_raw = cur_raw;
        index = edge + 1;

        if (!(prev_level && !cur_level))
            continue;

        const uint64_t frame_start = edge;
        const double first_mid = (double)frame_start + (bit_width - 1.0) / 2.0;
        const uint64_t start_mid = (uint64_t)ceil(first_mid);
        const uint64_t frame_needed_end = (uint64_t)ceil((double)frame_start +
            bit_width * (double)(1 + data_bits + (has_parity ? 1 : 0)) +
            bit_width * stop_bits);
        if (start_mid > decode_end || frame_needed_end > decode_end) {
            if (_is_capture_end)
                break;
            index = frame_start;
            last_raw = prev_raw;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        const bool start_bit = invert ? !get_sample(start_mid) : get_sample(start_mid);
        if (start_bit != false) {
            const uint64_t warn_end = min<uint64_t>(decode_end, start_mid + (uint64_t)ceil(bit_width / 2.0));
            push_native_annotation(start_mid, warn_end, 5, 1000,
                                   {QString("Frame error"), QString("Frame err"), QString("FE")});
            continue;
        }

        uint32_t value = 0;
        uint64_t last_data_mid = start_mid;
        bool truncated = false;
        for (int bit = 0; bit < data_bits; bit++) {
            const uint64_t sample = (uint64_t)ceil(first_mid + bit_width * (double)(1 + bit));
            if (sample > decode_end) {
                truncated = true;
                break;
            }

            bool level = get_sample(sample);
            if (invert)
                level = !level;

            if (msb_first)
                value = (value << 1) | (level ? 1 : 0);
            else if (level)
                value |= (1u << bit);

            last_data_mid = sample;
        }

        if (truncated) {
            if (_is_capture_end)
                break;
            index = frame_start;
            last_raw = prev_raw;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        const uint64_t stop_mid = (uint64_t)ceil(first_mid + bit_width * (double)(1 + data_bits + (has_parity ? 1 : 0)));
        if (stop_mid <= decode_end) {
            bool stop_level = get_sample(stop_mid);
            if (invert)
                stop_level = !stop_level;
            if (!stop_level) {
                const uint64_t warn_end = min<uint64_t>(decode_end, stop_mid + (uint64_t)ceil(bit_width / 2.0));
                push_native_annotation(stop_mid, warn_end, 5, 1000,
                                       {QString("Frame error"), QString("Frame err"), QString("FE")});
            }
        }

        const uint64_t halfbit = (uint64_t)ceil(bit_width / 2.0);
        uint64_t ann_start = frame_start;
        uint64_t ann_end = last_data_mid + (uint64_t)ceil((bit_width / 2.0) * (1.0 + stop_bits));
        if (show_startstop) {
            ann_start = start_mid > halfbit ? start_mid - halfbit : frame_start;
            ann_end = min<uint64_t>(decode_end, last_data_mid + halfbit);
        }
        ann_end = min<uint64_t>(ann_end, decode_end);

        push_native_annotation(ann_start, ann_end, 0, 108,
                               {native_uart_text(value, data_bits, display_format)});

        const uint64_t frame_end = frame_needed_end;
        if (frame_end > index && frame_end < decode_end) {
            index = frame_end;
            last_raw = get_sample(index - 1);
        }

        {
            std::lock_guard<std::mutex> lock(_output_mutex);
            _samples_decoded = index - decode_start;
        }

        if (index - last_notify >= notify_step) {
            last_notify = index;
            const uint64_t done_total = max<uint64_t>(decode_end - decode_start + 1, 1);
            _progress = (int)min<uint64_t>((index - decode_start) * 100 / done_total, 99);
            new_decode_data();
        }
    }

    _progress = 100;
    _is_decoding = false;
    {
        std::lock_guard<std::mutex> lock(_output_mutex);
        _samples_decoded = index > decode_start ? index - decode_start : 0;
    }
    new_decode_data();
    if (!_session->is_closed())
        decode_done();

    const auto native_elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - native_start_time).count();
    dsv_info("native uart decode done: ch=%d, annotations=%llu, elapsed=%lld ms",
             sig_index, (u64_t)_result_count, (long long)native_elapsed_ms);
    return true;
}

void DecoderStack::execute_decode_stack()
{  
	srd_session *session = NULL;
	srd_decoder_inst *prev_di = NULL;
    uint64_t decode_start = 0;
    uint64_t decode_end = 0;

	assert(_snapshot);

    // Get the intial sample count
    _sample_count = _snapshot->get_ring_sample_count();

    char *error = NULL;
    bool session_started = false;
    {
        std::lock_guard<std::mutex> lock(srd_session_mutex);

        srd_session_new(&session);
        if (session == NULL) {
            dsv_err("Failed to call srd_session_new()");
            assert(false);
            return;
        }

        for (auto dec : _stack) {
            srd_decoder_inst *const di = dec->create_decoder_inst(session);
            if (!di) {
                _error_message = L_S(STR_PAGE_MSG,
                    S_ID(IDS_MSG_DECODERSTACK_DECODE_STACK_ERROR),
                    "Failed to create decoder instance");
                srd_session_destroy(session);
                return;
            }

            if (prev_di)
                srd_inst_stack(session, prev_di, di);

            prev_di = di;
            decode_start = dec->decode_start();
            if (_session->is_realtime_refresh() == false)
                decode_end = min(dec->decode_end(), _sample_count - 1);
            else
                decode_end = max(dec->decode_end(), decode_end);
        }

        srd_session_metadata_set(session, SRD_CONF_SAMPLERATE,
            g_variant_new_uint64((uint64_t)_samplerate));
        srd_pd_output_callback_add(session, SRD_OUTPUT_ANN,
            DecoderStack::annotation_callback, _stask_stauts);

        session_started = srd_session_start(session, &error) == SRD_OK;
        if (!session_started) {
            if (error != NULL)
                _error_message = QString::fromLocal8Bit(error);
        }
    }

    dsv_info("decoder start sample:%llu, end sample:%llu, count:%llu",
            (u64_t)decode_start, (u64_t)decode_end,
            (u64_t)(decode_end - decode_start + 1));

    if (session_started) {
	       //need a lot time
        decode_data(decode_start, decode_end, session);
    }

	// Destroy the session
    if (error != NULL) {
        g_free(error);
    }

    {
        std::lock_guard<std::mutex> lock(srd_session_mutex);
	    srd_session_destroy(session);
    }
}

uint64_t DecoderStack::sample_count()
{
    if (_snapshot)
        return _snapshot->get_sample_count();
    else
        return 0;
}

uint64_t DecoderStack::sample_rate()
{
    return _samplerate;
}

//the decode callback, annotation object will be create
void DecoderStack::annotation_callback(srd_proto_data *pdata, void *self)
{
	assert(pdata);
	assert(self);

    struct decode_task_status *st = (decode_task_status*)self;

	DecoderStack *const d = st->_decoder;
	assert(d);

    if (st->_bStop){ 
        return;
    }
    if (d->_decoder_status == NULL){ 
        dsv_err("decode task was deleted.");
        assert(false);
    }
  
    if (d->_no_memory) {
        return;
    }

    Annotation *a = new Annotation(pdata, d->_decoder_status);
    if (a == NULL){
        d->_no_memory = true;
        return;     
    }
    d->_result_count++;

	// Find the row
	assert(pdata->pdo);
	assert(pdata->pdo->di);
	const srd_decoder *const decc = pdata->pdo->di->decoder;
	assert(decc);

    auto row_iter = d->_rows.end();
	
	// Try looking up the sub-row of this class
	const map<pair<const srd_decoder*, int>, Row>::const_iterator r =
        d->_class_rows.find(make_pair(decc, a->format()));
	if (r != d->_class_rows.end())
        row_iter = d->_rows.find((*r).second);
	else
	{
		// Failing that, use the decoder as a key
        row_iter = d->_rows.find(Row(decc));
	}

    assert(row_iter != d->_rows.end());
    if (row_iter == d->_rows.end()) {
        dsv_err("Unexpected annotation: decoder = 0x%x, format = %d", (void*)decc, a->format());
        assert(0);
        return;
    }

	// Add the annotation 
    if (!(*row_iter).second->push_annotation(a))
        d->_no_memory = true; 
}
 
void DecoderStack::frame_ended()
{ 
    _options_changed = true; 
}

int DecoderStack::list_rows_size()
{ 
    int rows_size = 0;
    for (auto i = _rows.begin(); i != _rows.end(); i++) {
        auto iter = _rows_lshow.find((*i).first);
        if (iter != _rows_lshow.end() && (*iter).second)
            rows_size++;
    }
    return rows_size;
}

bool DecoderStack::options_changed()
{
    return _options_changed;
}

void DecoderStack::set_options_changed(bool changed)
{
    _options_changed = changed;
}

bool DecoderStack::out_of_memory()
{
    return _no_memory;
}

void DecoderStack::set_mark_index(int64_t index)
{
    _mark_index = index;
}

int64_t DecoderStack::get_mark_index()
{
    return _mark_index;
}

const char* DecoderStack::get_root_decoder_id()
{
    if (_stack.size() > 0){
        decode::Decoder *dec = _stack.front();
        return dec->decoder()->id;
    }
    return NULL;
}

} // namespace data
} // namespace pv
