/* test_libFLAC - Unit tester for libFLAC
 * Copyright (C) 2002-2009  Josh Coalson
 * Copyright (C) 2011-2025  Xiph.Org Foundation
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "decoders.h"
#include "FLAC/assert.h"
#include "FLAC/stream_decoder.h"
#include "share/grabbag.h"
#include "share/compat.h"
#include "share/safe_str.h"
#include "test_libs_common/file_utils_flac.h"
#include "test_libs_common/metadata_utils.h"

typedef enum {
	LAYER_STREAM = 0, /* FLAC__stream_decoder_init_[ogg_]stream() without seeking */
	LAYER_SEEKABLE_STREAM, /* FLAC__stream_decoder_init_[ogg_]stream() with seeking */
	LAYER_FILE, /* FLAC__stream_decoder_init_[ogg_]FILE() */
	LAYER_FILENAME /* FLAC__stream_decoder_init_[ogg_]file() */
} Layer;

static const char * const LayerString[] = {
	"Stream",
	"Seekable Stream",
	"FILE*",
	"Filename"
};

typedef struct {
	Layer layer;
	FILE *file;
	char filename[512];
	uint32_t current_metadata_number;
	FLAC__bool got_audio;
	FLAC__bool ignore_errors;
	FLAC__bool error_occurred;
	FLAC__bool other_chain;
} StreamDecoderClientData;

static FLAC__StreamMetadata streaminfo_, padding_, seektable_, application1_, application2_, vorbiscomment_, cuesheet_, picture_, unknown_;
static FLAC__StreamMetadata *expected_metadata_sequence_[9];
static FLAC__StreamMetadata *expected_metadata_sequence_other_chain_[3];
static uint32_t num_expected_;
static uint32_t num_expected_other_chain_;
static FLAC__off_t flacfilesize_;
static uint32_t samples_;

static const char *flacfilename(FLAC__bool is_ogg, FLAC__bool is_chained_ogg)
{
	if(is_chained_ogg) return "metadata_chained.oga";
	return is_ogg? "metadata.oga" : "metadata.flac";
}

static FLAC__bool die_(const char *msg)
{
	printf("ERROR: %s\n", msg);
	return false;
}

static FLAC__bool die_s_(const char *msg, const FLAC__StreamDecoder *decoder)
{
	FLAC__StreamDecoderState state = FLAC__stream_decoder_get_state(decoder);

	if(msg)
		printf("FAILED, %s", msg);
	else
		printf("FAILED");

	printf(", state = %u (%s)\n", (uint32_t)state, FLAC__StreamDecoderStateString[state]);

	return false;
}

static void open_test_file(StreamDecoderClientData * pdcd, int is_ogg, int is_chained_ogg, const char * mode)
{
	pdcd->file = flac_fopen(flacfilename(is_ogg,is_chained_ogg), mode);
	safe_strncpy(pdcd->filename, flacfilename(is_ogg,is_chained_ogg), sizeof (pdcd->filename));
}

static void init_metadata_blocks_(void)
{
	mutils__init_metadata_blocks(&streaminfo_, &padding_, &seektable_, &application1_, &application2_, &vorbiscomment_, &cuesheet_, &picture_, &unknown_);
}

static void free_metadata_blocks_(void)
{
	mutils__free_metadata_blocks(&streaminfo_, &padding_, &seektable_, &application1_, &application2_, &vorbiscomment_, &cuesheet_, &picture_, &unknown_);
}

static FLAC__bool generate_file_(FLAC__bool is_ogg, FLAC__bool is_chained_ogg)
{
	FLAC__off_t filesize;
	printf("\n\ngenerating %sFLAC file for decoder tests...\n", is_chained_ogg? "chained Ogg " : is_ogg? "Ogg " : "");

	if(is_chained_ogg) {
		/* Create a different file as the first link */
		expected_metadata_sequence_other_chain_[0] = &streaminfo_;
		expected_metadata_sequence_other_chain_[1] = &vorbiscomment_;
		expected_metadata_sequence_other_chain_[2] = &unknown_;
		if(!file_utils__generate_flacfile(is_ogg, flacfilename(is_ogg,true), &flacfilesize_, samples_, &streaminfo_, expected_metadata_sequence_other_chain_+1, 2))
			return die_("creating the encoded file");
		file_utils__ogg_serial_number++;
		filesize=flacfilesize_;
	}

	num_expected_ = 0;
	expected_metadata_sequence_[num_expected_++] = &padding_;
	expected_metadata_sequence_[num_expected_++] = &seektable_;
	expected_metadata_sequence_[num_expected_++] = &application1_;
	expected_metadata_sequence_[num_expected_++] = &application2_;
	expected_metadata_sequence_[num_expected_++] = &vorbiscomment_;
	expected_metadata_sequence_[num_expected_++] = &cuesheet_;
	expected_metadata_sequence_[num_expected_++] = &picture_;
	expected_metadata_sequence_[num_expected_++] = &unknown_;
	/* WATCHOUT: for Ogg FLAC the encoder should move the VORBIS_COMMENT block to the front, right after STREAMINFO */

	if(!file_utils__generate_flacfile(is_ogg, flacfilename(is_ogg,false), &flacfilesize_, samples_, &streaminfo_, expected_metadata_sequence_, num_expected_))
		return die_("creating the encoded file");

	if(is_chained_ogg) {
		file_utils__append_file(flacfilename(is_ogg, true), flacfilename(is_ogg, false));
		flacfilesize_+=filesize;
	}

	return true;
}

static FLAC__StreamDecoderReadStatus stream_decoder_read_callback_(const FLAC__StreamDecoder *decoder, FLAC__byte buffer[], size_t *bytes, void *client_data)
{
	StreamDecoderClientData *dcd = (StreamDecoderClientData*)client_data;
	const size_t requested_bytes = *bytes;

	(void)decoder;

	if(0 == dcd) {
		printf("ERROR: client_data in read callback is NULL\n");
		return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
	}

	if(dcd->error_occurred)
		return FLAC__STREAM_DECODER_READ_STATUS_ABORT;

	if(feof(dcd->file)) {
		*bytes = 0;
		return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
	}
	else if(requested_bytes > 0) {
		*bytes = fread(buffer, 1, requested_bytes, dcd->file);
		if(*bytes == 0) {
			if(feof(dcd->file))
				return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
			else
				return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
		}
		else {
			return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
		}
	}
	else
		return FLAC__STREAM_DECODER_READ_STATUS_ABORT; /* abort to avoid a deadlock */
}

static FLAC__StreamDecoderSeekStatus stream_decoder_seek_callback_(const FLAC__StreamDecoder *decoder, FLAC__uint64 absolute_byte_offset, void *client_data)
{
	StreamDecoderClientData *dcd = (StreamDecoderClientData*)client_data;

	(void)decoder;

	if(0 == dcd) {
		printf("ERROR: client_data in seek callback is NULL\n");
		return FLAC__STREAM_DECODER_SEEK_STATUS_ERROR;
	}

	if(dcd->error_occurred)
		return FLAC__STREAM_DECODER_SEEK_STATUS_ERROR;

	if(fseeko(dcd->file, (FLAC__off_t)absolute_byte_offset, SEEK_SET) < 0) {
		dcd->error_occurred = true;
		return FLAC__STREAM_DECODER_SEEK_STATUS_ERROR;
	}

	return FLAC__STREAM_DECODER_SEEK_STATUS_OK;
}

static FLAC__StreamDecoderTellStatus stream_decoder_tell_callback_(const FLAC__StreamDecoder *decoder, FLAC__uint64 *absolute_byte_offset, void *client_data)
{
	StreamDecoderClientData *dcd = (StreamDecoderClientData*)client_data;
	FLAC__off_t offset;

	(void)decoder;

	if(0 == dcd) {
		printf("ERROR: client_data in tell callback is NULL\n");
		return FLAC__STREAM_DECODER_TELL_STATUS_ERROR;
	}

	if(dcd->error_occurred)
		return FLAC__STREAM_DECODER_TELL_STATUS_ERROR;

	offset = ftello(dcd->file);
	*absolute_byte_offset = (FLAC__uint64)offset;

	if(offset < 0) {
		dcd->error_occurred = true;
		return FLAC__STREAM_DECODER_TELL_STATUS_ERROR;
	}

	return FLAC__STREAM_DECODER_TELL_STATUS_OK;
}

static FLAC__StreamDecoderLengthStatus stream_decoder_length_callback_(const FLAC__StreamDecoder *decoder, FLAC__uint64 *stream_length, void *client_data)
{
	StreamDecoderClientData *dcd = (StreamDecoderClientData*)client_data;

	(void)decoder;

	if(0 == dcd) {
		printf("ERROR: client_data in length callback is NULL\n");
		return FLAC__STREAM_DECODER_LENGTH_STATUS_ERROR;
	}

	if(dcd->error_occurred)
		return FLAC__STREAM_DECODER_LENGTH_STATUS_ERROR;

	*stream_length = (FLAC__uint64)flacfilesize_;
	return FLAC__STREAM_DECODER_LENGTH_STATUS_OK;
}

static FLAC__bool stream_decoder_eof_callback_(const FLAC__StreamDecoder *decoder, void *client_data)
{
	StreamDecoderClientData *dcd = (StreamDecoderClientData*)client_data;

	(void)decoder;

	if(0 == dcd) {
		printf("ERROR: client_data in eof callback is NULL\n");
		return true;
	}

	if(dcd->error_occurred)
		return true;

	return feof(dcd->file);
}

static FLAC__StreamDecoderWriteStatus stream_decoder_write_callback_(const FLAC__StreamDecoder *decoder, const FLAC__Frame *frame, const FLAC__int32 * const buffer[], void *client_data)
{
	StreamDecoderClientData *dcd = (StreamDecoderClientData*)client_data;

	(void)decoder, (void)buffer;

	if(0 == dcd) {
		printf("ERROR: client_data in write callback is NULL\n");
		return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
	}

	if(dcd->error_occurred)
		return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;

	dcd->got_audio = true;

	if(
		(frame->header.number_type == FLAC__FRAME_NUMBER_TYPE_FRAME_NUMBER && frame->header.number.frame_number == 0) ||
		(frame->header.number_type == FLAC__FRAME_NUMBER_TYPE_SAMPLE_NUMBER && frame->header.number.sample_number == 0)
	) {
		printf("content... ");
		fflush(stdout);
	}

	return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static void stream_decoder_metadata_callback_(const FLAC__StreamDecoder *decoder, const FLAC__StreamMetadata *metadata, void *client_data)
{
	StreamDecoderClientData *dcd = (StreamDecoderClientData*)client_data;

	(void)decoder;

	if(0 == dcd) {
		printf("ERROR: client_data in metadata callback is NULL\n");
		return;
	}

	if(dcd->error_occurred)
		return;

	if (metadata->type == FLAC__METADATA_TYPE_APPLICATION) {
		printf ("%u ('%c%c%c%c')... ", dcd->current_metadata_number, metadata->data.application.id [0], metadata->data.application.id [1], metadata->data.application.id [2], metadata->data.application.id [3]);
	}
	else {
		printf("%u... ", dcd->current_metadata_number);
	}
	fflush(stdout);

	if((!dcd->other_chain && dcd->current_metadata_number >= num_expected_) ||
	   (dcd->other_chain && dcd->current_metadata_number >= num_expected_other_chain_)) {
		(void)die_("got more metadata blocks than expected");
		dcd->error_occurred = true;
	}
	else {
		if(!mutils__compare_block(dcd->other_chain ? expected_metadata_sequence_other_chain_[dcd->current_metadata_number] : expected_metadata_sequence_[dcd->current_metadata_number], metadata)) {
			(void)die_("metadata block mismatch");
			dcd->error_occurred = true;
		}
	}
	dcd->current_metadata_number++;
}

static void stream_decoder_error_callback_(const FLAC__StreamDecoder *decoder, FLAC__StreamDecoderErrorStatus status, void *client_data)
{
	StreamDecoderClientData *dcd = (StreamDecoderClientData*)client_data;

	(void)decoder;

	if(0 == dcd) {
		printf("ERROR: client_data in error callback is NULL\n");
		return;
	}

	if(!dcd->ignore_errors) {
		printf("ERROR: got error callback: err = %u (%s)\n", (uint32_t)status, FLAC__StreamDecoderErrorStatusString[status]);
		dcd->error_occurred = true;
	}
}

static FLAC__bool stream_decoder_test_respond_(FLAC__StreamDecoder *decoder, StreamDecoderClientData *dcd, FLAC__bool is_ogg, FLAC__bool is_chained_ogg)
{
	FLAC__StreamDecoderInitStatus init_status;

	if(!FLAC__stream_decoder_set_md5_checking(decoder, true))
		return die_s_("at FLAC__stream_decoder_set_md5_checking(), returned false", decoder);

	/* for FLAC__stream_encoder_init_FILE(), the FLAC__stream_encoder_finish() closes the file so we have to keep re-opening: */
	if(dcd->layer == LAYER_FILE) {
		printf("opening %sFLAC file... ", is_ogg? "Ogg ":"");
		open_test_file(dcd, is_ogg, is_chained_ogg, "rb");
		if(0 == dcd->file) {
			printf("ERROR (%s)\n", strerror(errno));
			return false;
		}
		printf("OK\n");
	}

	if(is_chained_ogg) {
		printf("testing FLAC__stream_decoder_set_decode_chained_stream()... ");
		if(!FLAC__stream_decoder_set_decode_chained_stream(decoder, true))
			return die_s_("returned false", decoder);
		printf("OK\n");
	}

	switch(dcd->layer) {
		case LAYER_STREAM:
			printf("testing FLAC__stream_decoder_init_%sstream()... ", is_ogg? "ogg_":"");
			init_status = is_ogg?
				FLAC__stream_decoder_init_ogg_stream(decoder, stream_decoder_read_callback_, /*seek_callback=*/0, /*tell_callback=*/0, /*length_callback=*/0, /*eof_callback=*/0, stream_decoder_write_callback_, stream_decoder_metadata_callback_, stream_decoder_error_callback_, dcd) :
				FLAC__stream_decoder_init_stream(decoder, stream_decoder_read_callback_, /*seek_callback=*/0, /*tell_callback=*/0, /*length_callback=*/0, /*eof_callback=*/0, stream_decoder_write_callback_, stream_decoder_metadata_callback_, stream_decoder_error_callback_, dcd)
			;
			break;
		case LAYER_SEEKABLE_STREAM:
			printf("testing FLAC__stream_decoder_init_%sstream()... ", is_ogg? "ogg_":"");
			init_status = is_ogg?
				FLAC__stream_decoder_init_ogg_stream(decoder, stream_decoder_read_callback_, stream_decoder_seek_callback_, stream_decoder_tell_callback_, stream_decoder_length_callback_, stream_decoder_eof_callback_, stream_decoder_write_callback_, stream_decoder_metadata_callback_, stream_decoder_error_callback_, dcd) :
				FLAC__stream_decoder_init_stream(decoder, stream_decoder_read_callback_, stream_decoder_seek_callback_, stream_decoder_tell_callback_, stream_decoder_length_callback_, stream_decoder_eof_callback_, stream_decoder_write_callback_, stream_decoder_metadata_callback_, stream_decoder_error_callback_, dcd);
			break;
		case LAYER_FILE:
			printf("testing FLAC__stream_decoder_init_%sFILE()... ", is_ogg? "ogg_":"");
			init_status = is_ogg?
				FLAC__stream_decoder_init_ogg_FILE(decoder, dcd->file, stream_decoder_write_callback_, stream_decoder_metadata_callback_, stream_decoder_error_callback_, dcd) :
				FLAC__stream_decoder_init_FILE(decoder, dcd->file, stream_decoder_write_callback_, stream_decoder_metadata_callback_, stream_decoder_error_callback_, dcd);
			break;
		case LAYER_FILENAME:
			printf("testing FLAC__stream_decoder_init_%sfile()... ", is_ogg? "ogg_":"");
			init_status = is_ogg?
				FLAC__stream_decoder_init_ogg_file(decoder, flacfilename(is_ogg,is_chained_ogg), stream_decoder_write_callback_, stream_decoder_metadata_callback_, stream_decoder_error_callback_, dcd) :
				FLAC__stream_decoder_init_file(decoder, flacfilename(is_ogg,is_chained_ogg), stream_decoder_write_callback_, stream_decoder_metadata_callback_, stream_decoder_error_callback_, dcd);
			break;
		default:
			die_("internal error 000");
			return false;
	}
	if(init_status != FLAC__STREAM_DECODER_INIT_STATUS_OK)
		return die_s_(0, decoder);
	printf("OK\n");

	dcd->current_metadata_number = 0;

	if(dcd->layer < LAYER_FILE && fseeko(dcd->file, 0, SEEK_SET) < 0) {
		printf("FAILED rewinding input, errno = %d\n", errno);
		return false;
	}

        if(is_chained_ogg) {
		dcd->other_chain = true;
                printf("skip first chain link with FLAC__stream_decoder_process_until_end_of_link()... ");
                if(!FLAC__stream_decoder_process_until_end_of_link(decoder))
                        return die_s_("returned false", decoder);
                printf("OK\n");
		dcd->other_chain = false;
		printf("checking whether metadata was returned by previous call... ");
		if(dcd->current_metadata_number != num_expected_other_chain_) {
			return die_s_("too few or too much metadata blocks returned", decoder);
		}
		printf("OK\n");

                printf("progress to next chain link with FLAC__stream_decoder_finish_link()... ");
                if(!FLAC__stream_decoder_finish_link(decoder))
                        return die_s_("returned false", decoder);
                printf("OK\n");

		dcd->current_metadata_number = 0;

        }

	printf("testing FLAC__stream_decoder_process_until_end_of_stream()... ");
	if(!FLAC__stream_decoder_process_until_end_of_stream(decoder))
		return die_s_("returned false", decoder);
	printf("OK\n");

        if(is_chained_ogg && dcd->layer != LAYER_STREAM) {
		dcd->current_metadata_number = 0;

		dcd->other_chain = true;
                printf("seek to first chain... ");
                if(!FLAC__stream_decoder_seek_absolute(decoder, 0))
                        return die_s_("returned false", decoder);
                printf("OK\n");
		dcd->other_chain = false;

		printf("checking whether metadata was returned by previous call... ");
		if(dcd->current_metadata_number != num_expected_other_chain_) {
			return die_s_("too few or too much metadata blocks returned", decoder);
		}
		printf("OK\n");

		dcd->current_metadata_number = 0;

		printf("testing FLAC__stream_decoder_seek_absolute(), seeking to second link... ");
		if(!FLAC__stream_decoder_seek_absolute(decoder, 512 * 1024))
			return die_s_("returned false", decoder);
		printf("OK\n");

		printf("checking whether metadata was returned by previous call... ");
		if(dcd->current_metadata_number != num_expected_) {
			return die_s_("too few or too much metadata blocks returned", decoder);
		}
		printf("OK\n");
        }

	printf("testing FLAC__stream_decoder_finish()... ");
	if(!FLAC__stream_decoder_finish(decoder))
		return die_s_("returned false", decoder);
	printf("OK\n");

	return true;
}

static FLAC__bool test_stream_decoder(Layer layer, FLAC__bool is_ogg, FLAC__bool is_chained_ogg)
{
	FLAC__StreamDecoder *decoder;
	FLAC__StreamDecoderInitStatus init_status;
	FLAC__StreamDecoderState state;
	StreamDecoderClientData decoder_client_data;
	FLAC__bool expect;
	FLAC__uint64 total_samples;

	decoder_client_data.layer = layer;
	decoder_client_data.other_chain = false;
	decoder_client_data.error_occurred = false;
	decoder_client_data.got_audio = false;
	decoder_client_data.ignore_errors = false;

	printf("\n+++ libFLAC unit test: FLAC__StreamDecoder (layer: %s, format: %s)\n\n", LayerString[layer], is_chained_ogg? "chained Ogg FLAC" : is_ogg? "Ogg FLAC" : "FLAC");

	printf("testing FLAC__stream_decoder_new()... ");
	decoder = FLAC__stream_decoder_new();
	if(0 == decoder) {
		printf("FAILED, returned NULL\n");
		return false;
	}
	printf("OK\n");

	printf("testing FLAC__stream_decoder_delete()... ");
	FLAC__stream_decoder_delete(decoder);
	printf("OK\n");

	printf("testing FLAC__stream_decoder_new()... ");
	decoder = FLAC__stream_decoder_new();
	if(0 == decoder) {
		printf("FAILED, returned NULL\n");
		return false;
	}
	printf("OK\n");

	if(layer < LAYER_FILENAME) {
		printf("opening %sFLAC file... ", is_ogg? "Ogg ":"");
		open_test_file(&decoder_client_data, is_ogg, is_chained_ogg, "rb");
		if(0 == decoder_client_data.file) {
			printf("ERROR (%s)\n", strerror(errno));
			return false;
		}
		printf("OK\n");
	}

	if(is_chained_ogg) {
		printf("testing FLAC__stream_decoder_set_decode_chained_stream()... ");
		if(!FLAC__stream_decoder_set_decode_chained_stream(decoder, true))
			return die_s_("returned false", decoder);
		printf("OK\n");
	}

	switch(layer) {
		case LAYER_STREAM:
			printf("testing FLAC__stream_decoder_init_%sstream()... ", is_ogg? "ogg_":"");
			init_status = is_ogg?
				FLAC__stream_decoder_init_ogg_stream(decoder, stream_decoder_read_callback_, /*seek_callback=*/0, /*tell_callback=*/0, /*length_callback=*/0, /*eof_callback=*/0, stream_decoder_write_callback_, stream_decoder_metadata_callback_, stream_decoder_error_callback_, &decoder_client_data) :
				FLAC__stream_decoder_init_stream(decoder, stream_decoder_read_callback_, /*seek_callback=*/0, /*tell_callback=*/0, /*length_callback=*/0, /*eof_callback=*/0, stream_decoder_write_callback_, stream_decoder_metadata_callback_, stream_decoder_error_callback_, &decoder_client_data);
			break;
		case LAYER_SEEKABLE_STREAM:
			printf("testing FLAC__stream_decoder_init_%sstream()... ", is_ogg? "ogg_":"");
			init_status = is_ogg?
				FLAC__stream_decoder_init_ogg_stream(decoder, stream_decoder_read_callback_, stream_decoder_seek_callback_, stream_decoder_tell_callback_, stream_decoder_length_callback_, stream_decoder_eof_callback_, stream_decoder_write_callback_, stream_decoder_metadata_callback_, stream_decoder_error_callback_, &decoder_client_data) :
				FLAC__stream_decoder_init_stream(decoder, stream_decoder_read_callback_, stream_decoder_seek_callback_, stream_decoder_tell_callback_, stream_decoder_length_callback_, stream_decoder_eof_callback_, stream_decoder_write_callback_, stream_decoder_metadata_callback_, stream_decoder_error_callback_, &decoder_client_data);
			break;
		case LAYER_FILE:
			printf("testing FLAC__stream_decoder_init_%sFILE()... ", is_ogg? "ogg_":"");
			init_status = is_ogg?
				FLAC__stream_decoder_init_ogg_FILE(decoder, decoder_client_data.file, stream_decoder_write_callback_, stream_decoder_metadata_callback_, stream_decoder_error_callback_, &decoder_client_data) :
				FLAC__stream_decoder_init_FILE(decoder, decoder_client_data.file, stream_decoder_write_callback_, stream_decoder_metadata_callback_, stream_decoder_error_callback_, &decoder_client_data);
			break;
		case LAYER_FILENAME:
			printf("testing FLAC__stream_decoder_init_%sfile()... ", is_ogg? "ogg_":"");
			init_status = is_ogg?
				FLAC__stream_decoder_init_ogg_file(decoder, flacfilename(is_ogg,is_chained_ogg), stream_decoder_write_callback_, stream_decoder_metadata_callback_, stream_decoder_error_callback_, &decoder_client_data) :
				FLAC__stream_decoder_init_file(decoder, flacfilename(is_ogg,is_chained_ogg), stream_decoder_write_callback_, stream_decoder_metadata_callback_, stream_decoder_error_callback_, &decoder_client_data);
			break;
		default:
			die_("internal error 009");
			return false;
	}
	if(init_status != FLAC__STREAM_DECODER_INIT_STATUS_OK)
		return die_s_(0, decoder);
	printf("OK\n");

	printf("testing FLAC__stream_decoder_get_link_lengths()... ");
	if(is_chained_ogg) {
		if(FLAC__stream_decoder_get_link_lengths(decoder, NULL) != FLAC__STREAM_DECODER_GET_LINK_LENGTHS_NOT_INDEXED)
			return die_s_("returned incorrectly", decoder);
	}
	else{ /* not chained ogg */
		if(FLAC__stream_decoder_get_link_lengths(decoder, NULL) != FLAC__STREAM_DECODER_GET_LINK_LENGTHS_INVALID)
			return die_s_("returned incorrectly", decoder);
	}
	printf("OK\n");

	printf("testing FLAC__stream_decoder_find_total_samples... ");
	total_samples = FLAC__stream_decoder_find_total_samples(decoder);
	printf("Number of samples returned is %" PRIu64 "... ",total_samples);
	if((layer == LAYER_STREAM && total_samples != 0) ||
	   (layer != LAYER_STREAM && is_chained_ogg && total_samples != (samples_ * 2)) ||
	   (layer != LAYER_STREAM && !is_chained_ogg && total_samples != samples_))
		return die_s_("returned wrong number of samples", decoder);
	printf("OK\n");

	if(is_chained_ogg) {
		printf("testing FLAC__stream_decoder_get_link_lengths()... ");
		if(layer == LAYER_STREAM) {
			if(FLAC__stream_decoder_get_link_lengths(decoder, NULL) != FLAC__STREAM_DECODER_GET_LINK_LENGTHS_NOT_INDEXED)
				return die_s_("returned incorrectly", decoder);
		}
		else {
			int32_t retval = FLAC__stream_decoder_get_link_lengths(decoder, NULL);
			printf("Number of links returned is %d\n",retval);
			if(retval != 2)
				return die_s_("returned incorrectly", decoder);

		}
		printf("OK\n");
	}

	if(layer < LAYER_FILE) /* for LAYER_FILE, FLAC__stream_decoder_finish() closes the file */
		fclose(decoder_client_data.file);

	printf("testing FLAC__stream_decoder_finish()... ");
	FLAC__stream_decoder_finish(decoder);
	printf("OK\n");

	switch(layer) {
		case LAYER_STREAM:
		case LAYER_SEEKABLE_STREAM:
			printf("testing FLAC__stream_decoder_init_%sstream()... ", is_ogg? "ogg_":"");
			init_status = is_ogg?
				FLAC__stream_decoder_init_ogg_stream(decoder, 0, 0, 0, 0, 0, 0, 0, 0, 0) :
				FLAC__stream_decoder_init_stream(decoder, 0, 0, 0, 0, 0, 0, 0, 0, 0);
			break;
		case LAYER_FILE:
			printf("testing FLAC__stream_decoder_init_%sFILE()... ", is_ogg? "ogg_":"");
			init_status = is_ogg?
				FLAC__stream_decoder_init_ogg_FILE(decoder, stdin, 0, 0, 0, 0) :
				FLAC__stream_decoder_init_FILE(decoder, stdin, 0, 0, 0, 0);
			break;
		case LAYER_FILENAME:
			printf("testing FLAC__stream_decoder_init_%sfile()... ", is_ogg? "ogg_":"");
			init_status = is_ogg?
				FLAC__stream_decoder_init_ogg_file(decoder, flacfilename(is_ogg,is_chained_ogg), 0, 0, 0, 0) :
				FLAC__stream_decoder_init_file(decoder, flacfilename(is_ogg,is_chained_ogg), 0, 0, 0, 0);
			break;
		default:
			die_("internal error 003");
			return false;
	}
	if(init_status != FLAC__STREAM_DECODER_INIT_STATUS_INVALID_CALLBACKS)
		return die_s_(0, decoder);
	printf("OK\n");

	printf("testing FLAC__stream_decoder_delete()... ");
	FLAC__stream_decoder_delete(decoder);
	printf("OK\n");

	num_expected_ = 0;
	expected_metadata_sequence_[num_expected_++] = &streaminfo_;

	printf("testing FLAC__stream_decoder_new()... ");
	decoder = FLAC__stream_decoder_new();
	if(0 == decoder) {
		printf("FAILED, returned NULL\n");
		return false;
	}
	printf("OK\n");

	if(is_ogg) {
		printf("testing FLAC__stream_decoder_set_ogg_serial_number()... ");
		if(!FLAC__stream_decoder_set_ogg_serial_number(decoder, file_utils__ogg_serial_number))
			return die_s_("returned false", decoder);
		printf("OK\n");
	}

	printf("testing FLAC__stream_decoder_set_md5_checking()... ");
	if(!FLAC__stream_decoder_set_md5_checking(decoder, true))
		return die_s_("returned false", decoder);
	printf("OK\n");

	if(is_chained_ogg) {
		printf("testing FLAC__stream_decoder_set_decode_chained_stream()... ");
		if(!FLAC__stream_decoder_set_decode_chained_stream(decoder, true))
			return die_s_("returned false", decoder);
		printf("OK\n");
	}

	if(layer < LAYER_FILENAME) {
		printf("opening %sFLAC file... ", is_ogg? "Ogg ":"");
		open_test_file(&decoder_client_data, is_ogg, is_chained_ogg, "rb");
		if(0 == decoder_client_data.file) {
			printf("ERROR (%s)\n", strerror(errno));
			return false;
		}
		printf("OK\n");
	}

	switch(layer) {
		case LAYER_STREAM:
			printf("testing FLAC__stream_decoder_init_%sstream()... ", is_ogg? "ogg_":"");
			init_status = is_ogg?
				FLAC__stream_decoder_init_ogg_stream(decoder, stream_decoder_read_callback_, /*seek_callback=*/0, /*tell_callback=*/0, /*length_callback=*/0, /*eof_callback=*/0, stream_decoder_write_callback_, stream_decoder_metadata_callback_, stream_decoder_error_callback_, &decoder_client_data) :
				FLAC__stream_decoder_init_stream(decoder, stream_decoder_read_callback_, /*seek_callback=*/0, /*tell_callback=*/0, /*length_callback=*/0, /*eof_callback=*/0, stream_decoder_write_callback_, stream_decoder_metadata_callback_, stream_decoder_error_callback_, &decoder_client_data);
			break;
		case LAYER_SEEKABLE_STREAM:
			printf("testing FLAC__stream_decoder_init_%sstream()... ", is_ogg? "ogg_":"");
			init_status = is_ogg?
				FLAC__stream_decoder_init_ogg_stream(decoder, stream_decoder_read_callback_, stream_decoder_seek_callback_, stream_decoder_tell_callback_, stream_decoder_length_callback_, stream_decoder_eof_callback_, stream_decoder_write_callback_, stream_decoder_metadata_callback_, stream_decoder_error_callback_, &decoder_client_data) :
				FLAC__stream_decoder_init_stream(decoder, stream_decoder_read_callback_, stream_decoder_seek_callback_, stream_decoder_tell_callback_, stream_decoder_length_callback_, stream_decoder_eof_callback_, stream_decoder_write_callback_, stream_decoder_metadata_callback_, stream_decoder_error_callback_, &decoder_client_data);
			break;
		case LAYER_FILE:
			printf("testing FLAC__stream_decoder_init_%sFILE()... ", is_ogg? "ogg_":"");
			init_status = is_ogg?
				FLAC__stream_decoder_init_ogg_FILE(decoder, decoder_client_data.file, stream_decoder_write_callback_, stream_decoder_metadata_callback_, stream_decoder_error_callback_, &decoder_client_data) :
				FLAC__stream_decoder_init_FILE(decoder, decoder_client_data.file, stream_decoder_write_callback_, stream_decoder_metadata_callback_, stream_decoder_error_callback_, &decoder_client_data);
			break;
		case LAYER_FILENAME:
			printf("testing FLAC__stream_decoder_init_%sfile()... ", is_ogg? "ogg_":"");
			init_status = is_ogg?
				FLAC__stream_decoder_init_ogg_file(decoder, flacfilename(is_ogg,is_chained_ogg), stream_decoder_write_callback_, stream_decoder_metadata_callback_, stream_decoder_error_callback_, &decoder_client_data) :
				FLAC__stream_decoder_init_file(decoder, flacfilename(is_ogg,is_chained_ogg), stream_decoder_write_callback_, stream_decoder_metadata_callback_, stream_decoder_error_callback_, &decoder_client_data);
			break;
		default:
			die_("internal error 009");
			return false;
	}
	if(init_status != FLAC__STREAM_DECODER_INIT_STATUS_OK)
		return die_s_(0, decoder);
	printf("OK\n");

	decoder_client_data.current_metadata_number = 0;
	decoder_client_data.ignore_errors = false;
	decoder_client_data.error_occurred = false;

	if(is_chained_ogg) {
		printf("testing FLAC__stream_decoder_process_until_end_of_metadata()... ");
		if(!FLAC__stream_decoder_process_until_end_of_metadata(decoder))
			return die_s_("returned false", decoder);
		printf("OK\n");

		printf("checking whether metadata was returned by previous call... ");
		if(decoder_client_data.current_metadata_number != 1) {
			return die_s_("no metadata returned", decoder);
		}
		printf("OK\n");

		decoder_client_data.current_metadata_number = 0;

		printf("skip first chain link with FLAC__stream_decoder_skip_single_link()... ");
		if(!FLAC__stream_decoder_skip_single_link(decoder))
			return die_s_("returned false", decoder);
		printf("OK\n");

		printf("skip second chain link with FLAC__stream_decoder_skip_single_link()... ");
		if(!FLAC__stream_decoder_skip_single_link(decoder))
			return die_s_("returned false", decoder);
		printf("OK\n");

		printf("checking whether no metadata was returned by previous call... ");
		if(decoder_client_data.current_metadata_number != 0) {
			return die_s_("metadata returned", decoder);
		}
		printf("OK\n");

		printf("testing FLAC__stream_decoder_get_state()... ");
		state = FLAC__stream_decoder_get_state(decoder);
		if(state != FLAC__STREAM_DECODER_END_OF_STREAM) {
			return die_s_("wrong state", decoder);
		}
		printf("returned state = %u (%s)... OK\n", state, FLAC__StreamDecoderStateString[state]);

		printf("testing FLAC__stream_decoder_get_link_lengths()... ");
		{
			FLAC__uint64 *link_lengths;
			int32_t retval = FLAC__stream_decoder_get_link_lengths(decoder, &link_lengths);
			printf("Number of links returned is %d\n",retval);
			printf("Number of samples is %" PRIu64 " and %" PRIu64 "\n",link_lengths[0],link_lengths[1]);
			if(retval != 2)
				return die_s_("returned incorrectly", decoder);
			free(link_lengths);

		}
		printf("OK\n");


		printf("testing FLAC__stream_decoder_reset()... ");
		if(!FLAC__stream_decoder_reset(decoder)) {
			state = FLAC__stream_decoder_get_state(decoder);
			printf("FAILED, returned false, state = %u (%s)\n", state, FLAC__StreamDecoderStateString[state]);
			return false;
		}
		printf("OK\n");

		if(layer == LAYER_STREAM) {
			/* after a reset() we have to rewind the input ourselves */
			printf("rewinding input... ");
			if(fseeko(decoder_client_data.file, 0, SEEK_SET) < 0) {
				printf("FAILED, errno = %d\n", errno);
				return false;
			}
			printf("OK\n");
		}

		printf("process first chain link with FLAC__stream_decoder_process_until_end_of_link()... ");
		if(!FLAC__stream_decoder_process_until_end_of_link(decoder))
			return die_s_("returned false", decoder);
		printf("OK\n");

		printf("checking whether metadata was returned by previous call... ");
		if(decoder_client_data.current_metadata_number != 1) {
			return die_s_("no metadata returned", decoder);
		}
		printf("OK\n");

		decoder_client_data.current_metadata_number = 0;

		printf("testing FLAC__stream_decoder_get_state()... ");
		state = FLAC__stream_decoder_get_state(decoder);
		if(state != FLAC__STREAM_DECODER_END_OF_LINK) {
			return die_s_("wrong state", decoder);
		}
		printf("returned state = %u (%s)... OK\n", state, FLAC__StreamDecoderStateString[state]);

		printf("progress to next chain link with FLAC__stream_decoder_finish_link()... ");
		if(!FLAC__stream_decoder_finish_link(decoder))
			return die_s_("returned false", decoder);
		printf("OK\n");

	}

	printf("testing FLAC__stream_decoder_get_state()... ");
	state = FLAC__stream_decoder_get_state(decoder);
	printf("returned state = %u (%s)... OK\n", state, FLAC__StreamDecoderStateString[state]);

	printf("testing FLAC__stream_decoder_get_md5_checking()... ");
	if(!FLAC__stream_decoder_get_md5_checking(decoder)) {
		printf("FAILED, returned false, expected true\n");
		return false;
	}
	printf("OK\n");

	printf("testing FLAC__stream_decoder_process_until_end_of_metadata()... ");
	if(!FLAC__stream_decoder_process_until_end_of_metadata(decoder))
		return die_s_("returned false", decoder);
	printf("OK\n");

	printf("checking whether metadata was returned by previous call... ");
	if(decoder_client_data.current_metadata_number != 1) {
		return die_s_("no metadata returned", decoder);
	}
	printf("OK\n");

	decoder_client_data.got_audio = false;

	printf("testing FLAC__stream_decoder_process_single()... ");
	if(!FLAC__stream_decoder_process_single(decoder))
		return die_s_("returned false", decoder);
	printf("OK\n");

	printf("checking whether processing returned audio... ");
	if(!decoder_client_data.got_audio) {
		return die_s_("no audio returned", decoder);
	}
	printf("OK\n");

	printf("testing FLAC__stream_decoder_skip_single_frame()... ");
	if(!FLAC__stream_decoder_skip_single_frame(decoder))
		return die_s_("returned false", decoder);
	printf("OK\n");

	if(layer < LAYER_FILE) {
		printf("testing FLAC__stream_decoder_flush()... ");
		if(!FLAC__stream_decoder_flush(decoder))
			return die_s_("returned false", decoder);
		printf("OK\n");

		decoder_client_data.ignore_errors = true;
		printf("testing FLAC__stream_decoder_process_single()... ");
		if(!FLAC__stream_decoder_process_single(decoder))
			return die_s_("returned false", decoder);
		printf("OK\n");
		decoder_client_data.ignore_errors = false;
	}

	if(is_chained_ogg)
		decoder_client_data.current_metadata_number = 0;

	expect = (layer != LAYER_STREAM);
	printf("testing FLAC__stream_decoder_seek_absolute()... ");
	if(FLAC__stream_decoder_seek_absolute(decoder, 0) != expect)
		return die_s_(expect? "returned false" : "returned true", decoder);
	printf("OK\n");

	if(is_chained_ogg) {
		if(layer != LAYER_STREAM) {
			printf("checking whether seeking to different link returns metadata... ");
			if(decoder_client_data.current_metadata_number != 1) {
				return die_s_("no metadata returned", decoder);
			}
			printf("OK\n");
			decoder_client_data.current_metadata_number = 0;
		}

		printf("testing FLAC__stream_decoder_seek_absolute(), seeking to second link... ");
		if(FLAC__stream_decoder_seek_absolute(decoder, 512 * 1024) != expect)
			return die_s_(expect? "returned false" : "returned true", decoder);
		printf("OK\n");

		if(layer != LAYER_STREAM) {
			printf("checking whether seeking to different link returns metadata... ");
			if(decoder_client_data.current_metadata_number != 1) {
				return die_s_("no metadata returned", decoder);
			}
			decoder_client_data.current_metadata_number = 0;
			printf("OK\n");
		}
	}

	decoder_client_data.got_audio = false;

	printf("testing FLAC__stream_decoder_process_until_end_of_stream()... ");
	if(!FLAC__stream_decoder_process_until_end_of_stream(decoder))
		return die_s_("returned false", decoder);
	printf("OK\n");

	printf("checking whether processing returned audio... ");
	if(!decoder_client_data.got_audio) {
		return die_s_("no audio returned", decoder);
	}
	printf("OK\n");

	expect = (layer != LAYER_STREAM);
	printf("testing FLAC__stream_decoder_seek_absolute()... ");
	if(FLAC__stream_decoder_seek_absolute(decoder, 0) != expect)
		return die_s_(expect? "returned false" : "returned true", decoder);
	printf("OK\n");

	printf("testing FLAC__stream_decoder_get_channels()... ");
	{
		uint32_t channels = FLAC__stream_decoder_get_channels(decoder);
		if(channels != streaminfo_.data.stream_info.channels) {
			printf("FAILED, returned %u, expected %u\n", channels, streaminfo_.data.stream_info.channels);
			return false;
		}
	}
	printf("OK\n");

	printf("testing FLAC__stream_decoder_get_bits_per_sample()... ");
	{
		uint32_t bits_per_sample = FLAC__stream_decoder_get_bits_per_sample(decoder);
		if(bits_per_sample != streaminfo_.data.stream_info.bits_per_sample) {
			printf("FAILED, returned %u, expected %u\n", bits_per_sample, streaminfo_.data.stream_info.bits_per_sample);
			return false;
		}
	}
	printf("OK\n");

	printf("testing FLAC__stream_decoder_get_sample_rate()... ");
	{
		uint32_t sample_rate = FLAC__stream_decoder_get_sample_rate(decoder);
		if(sample_rate != streaminfo_.data.stream_info.sample_rate) {
			printf("FAILED, returned %u, expected %u\n", sample_rate, streaminfo_.data.stream_info.sample_rate);
			return false;
		}
	}
	printf("OK\n");

	printf("testing FLAC__stream_decoder_get_blocksize()... ");
	{
		uint32_t blocksize = FLAC__stream_decoder_get_blocksize(decoder);
		/* value could be anything since we're at the last block, so accept any reasonable answer */
		printf("returned %u... %s\n", blocksize, blocksize>0? "OK" : "FAILED");
		if(blocksize == 0)
			return false;
	}

	printf("testing FLAC__stream_decoder_get_channel_assignment()... ");
	{
		FLAC__ChannelAssignment ca = FLAC__stream_decoder_get_channel_assignment(decoder);
		printf("returned %u (%s)... OK\n", (uint32_t)ca, FLAC__ChannelAssignmentString[ca]);
	}

	printf("testing FLAC__stream_decoder_reset()... ");
	if(!FLAC__stream_decoder_reset(decoder)) {
		state = FLAC__stream_decoder_get_state(decoder);
		printf("FAILED, returned false, state = %u (%s)\n", state, FLAC__StreamDecoderStateString[state]);
		return false;
	}
	printf("OK\n");

	if(layer == LAYER_STREAM) {
		/* after a reset() we have to rewind the input ourselves */
		printf("rewinding input... ");
		if(fseeko(decoder_client_data.file, 0, SEEK_SET) < 0) {
			printf("FAILED, errno = %d\n", errno);
			return false;
		}
		printf("OK\n");
	}
	decoder_client_data.current_metadata_number = 0;

	if(is_chained_ogg) {
		printf("skip first chain link with FLAC__stream_decoder_skip_single_link()... ");
		if(!FLAC__stream_decoder_skip_single_link(decoder))
			return die_s_("returned false", decoder);
		printf("OK\n");

		printf("skip second chain link with FLAC__stream_decoder_skip_single_link()... ");
		if(!FLAC__stream_decoder_skip_single_link(decoder))
			return die_s_("returned false", decoder);
		printf("OK\n");

		printf("checking whether skipping link returns no metadata... ");
		if(decoder_client_data.current_metadata_number != 0) {
			return die_s_("metadata returned", decoder);
		}
		printf("OK\n");

		printf("testing FLAC__stream_decoder_get_state()... ");
		state = FLAC__stream_decoder_get_state(decoder);
		if(state != FLAC__STREAM_DECODER_END_OF_STREAM) {
			return die_s_("wrong state", decoder);
		}
		printf("returned state = %u (%s)... OK\n", state, FLAC__StreamDecoderStateString[state]);

		printf("testing FLAC__stream_decoder_reset()... ");
		if(!FLAC__stream_decoder_reset(decoder)) {
			state = FLAC__stream_decoder_get_state(decoder);
			printf("FAILED, returned false, state = %u (%s)\n", state, FLAC__StreamDecoderStateString[state]);
			return false;
		}
		printf("OK\n");

		if(layer == LAYER_STREAM) {
			/* after a reset() we have to rewind the input ourselves */
			printf("rewinding input... ");
			if(fseeko(decoder_client_data.file, 0, SEEK_SET) < 0) {
				printf("FAILED, errno = %d\n", errno);
				return false;
			}
			printf("OK\n");
		}

		printf("skip first chain link with FLAC__stream_decoder_skip_single_link()... ");
		if(!FLAC__stream_decoder_skip_single_link(decoder))
			return die_s_("returned false", decoder);
		printf("OK\n");
	}

	decoder_client_data.got_audio = false;

	printf("testing FLAC__stream_decoder_process_until_end_of_stream()... ");
	if(!FLAC__stream_decoder_process_until_end_of_stream(decoder))
		return die_s_("returned false", decoder);
	printf("OK\n");

	printf("checking whether processing returned audio... ");
	if(!decoder_client_data.got_audio) {
		return die_s_("no audio returned", decoder);
	}
	printf("OK\n");

	printf("testing FLAC__stream_decoder_finish()... ");
	if(!FLAC__stream_decoder_finish(decoder))
		return die_s_("returned false", decoder);
	printf("OK\n");

	/*
	 * respond all
	 */

	printf("testing FLAC__stream_decoder_set_metadata_respond_all()... ");
	if(!FLAC__stream_decoder_set_metadata_respond_all(decoder))
		return die_s_("returned false", decoder);
	printf("OK\n");

	num_expected_ = 0;
	num_expected_other_chain_ = 0;
	if(is_ogg) { /* encoder moves vorbis comment after streaminfo according to ogg mapping. Also removes the seektable */
		expected_metadata_sequence_[num_expected_++] = &streaminfo_;
		expected_metadata_sequence_[num_expected_++] = &vorbiscomment_;
		expected_metadata_sequence_[num_expected_++] = &padding_;
		expected_metadata_sequence_[num_expected_++] = &application1_;
		expected_metadata_sequence_[num_expected_++] = &application2_;
		expected_metadata_sequence_[num_expected_++] = &cuesheet_;
		expected_metadata_sequence_[num_expected_++] = &picture_;
		expected_metadata_sequence_[num_expected_++] = &unknown_;

		expected_metadata_sequence_other_chain_[num_expected_other_chain_++] = &streaminfo_;
		expected_metadata_sequence_other_chain_[num_expected_other_chain_++] = &vorbiscomment_;
		expected_metadata_sequence_other_chain_[num_expected_other_chain_++] = &unknown_;
	}
	else {
		expected_metadata_sequence_[num_expected_++] = &streaminfo_;
		expected_metadata_sequence_[num_expected_++] = &padding_;
		expected_metadata_sequence_[num_expected_++] = &seektable_;
		expected_metadata_sequence_[num_expected_++] = &application1_;
		expected_metadata_sequence_[num_expected_++] = &application2_;
		expected_metadata_sequence_[num_expected_++] = &vorbiscomment_;
		expected_metadata_sequence_[num_expected_++] = &cuesheet_;
		expected_metadata_sequence_[num_expected_++] = &picture_;
		expected_metadata_sequence_[num_expected_++] = &unknown_;
	}

	if(!stream_decoder_test_respond_(decoder, &decoder_client_data, is_ogg, is_chained_ogg))
		return false;

	/*
	 * ignore all
	 */

	printf("testing FLAC__stream_decoder_set_metadata_ignore_all()... ");
	if(!FLAC__stream_decoder_set_metadata_ignore_all(decoder))
		return die_s_("returned false", decoder);
	printf("OK\n");

	num_expected_ = 0;
	num_expected_other_chain_ = 0;

	if(!stream_decoder_test_respond_(decoder, &decoder_client_data, is_ogg, is_chained_ogg))
		return false;

	/*
	 * respond all, ignore VORBIS_COMMENT
	 */

	printf("testing FLAC__stream_decoder_set_metadata_respond_all()... ");
	if(!FLAC__stream_decoder_set_metadata_respond_all(decoder))
		return die_s_("returned false", decoder);
	printf("OK\n");

	printf("testing FLAC__stream_decoder_set_metadata_ignore(VORBIS_COMMENT)... ");
	if(!FLAC__stream_decoder_set_metadata_ignore(decoder, FLAC__METADATA_TYPE_VORBIS_COMMENT))
		return die_s_("returned false", decoder);
	printf("OK\n");

	num_expected_ = 0;
	num_expected_other_chain_ = 0;
	expected_metadata_sequence_[num_expected_++] = &streaminfo_;
	expected_metadata_sequence_[num_expected_++] = &padding_;
	if(!is_ogg) /* encoder removes seektable for ogg */
		expected_metadata_sequence_[num_expected_++] = &seektable_;
	expected_metadata_sequence_[num_expected_++] = &application1_;
	expected_metadata_sequence_[num_expected_++] = &application2_;
	expected_metadata_sequence_[num_expected_++] = &cuesheet_;
	expected_metadata_sequence_[num_expected_++] = &picture_;
	expected_metadata_sequence_[num_expected_++] = &unknown_;

	expected_metadata_sequence_other_chain_[num_expected_other_chain_++] = &streaminfo_;
	expected_metadata_sequence_other_chain_[num_expected_other_chain_++] = &unknown_;

	if(!stream_decoder_test_respond_(decoder, &decoder_client_data, is_ogg, is_chained_ogg))
		return false;

	/*
	 * respond all, ignore APPLICATION
	 */

	printf("testing FLAC__stream_decoder_set_metadata_respond_all()... ");
	if(!FLAC__stream_decoder_set_metadata_respond_all(decoder))
		return die_s_("returned false", decoder);
	printf("OK\n");

	printf("testing FLAC__stream_decoder_set_metadata_ignore(APPLICATION)... ");
	if(!FLAC__stream_decoder_set_metadata_ignore(decoder, FLAC__METADATA_TYPE_APPLICATION))
		return die_s_("returned false", decoder);
	printf("OK\n");

	num_expected_ = 0;
	num_expected_other_chain_ = 0;
	if(is_ogg) { /* encoder moves vorbis comment after streaminfo according to ogg mapping. Also removes the seektable */
		expected_metadata_sequence_[num_expected_++] = &streaminfo_;
		expected_metadata_sequence_[num_expected_++] = &vorbiscomment_;
		expected_metadata_sequence_[num_expected_++] = &padding_;
		expected_metadata_sequence_[num_expected_++] = &cuesheet_;
		expected_metadata_sequence_[num_expected_++] = &picture_;
		expected_metadata_sequence_[num_expected_++] = &unknown_;

		expected_metadata_sequence_other_chain_[num_expected_other_chain_++] = &streaminfo_;
		expected_metadata_sequence_other_chain_[num_expected_other_chain_++] = &vorbiscomment_;
		expected_metadata_sequence_other_chain_[num_expected_other_chain_++] = &unknown_;
	}
	else {
		expected_metadata_sequence_[num_expected_++] = &streaminfo_;
		expected_metadata_sequence_[num_expected_++] = &padding_;
		expected_metadata_sequence_[num_expected_++] = &seektable_;
		expected_metadata_sequence_[num_expected_++] = &vorbiscomment_;
		expected_metadata_sequence_[num_expected_++] = &cuesheet_;
		expected_metadata_sequence_[num_expected_++] = &picture_;
		expected_metadata_sequence_[num_expected_++] = &unknown_;
	}

	if(!stream_decoder_test_respond_(decoder, &decoder_client_data, is_ogg, is_chained_ogg))
		return false;

	/*
	 * respond all, ignore APPLICATION id of app#1
	 */

	printf("testing FLAC__stream_decoder_set_metadata_respond_all()... ");
	if(!FLAC__stream_decoder_set_metadata_respond_all(decoder))
		return die_s_("returned false", decoder);
	printf("OK\n");

	printf("testing FLAC__stream_decoder_set_metadata_ignore_application(of app block #1)... ");
	if(!FLAC__stream_decoder_set_metadata_ignore_application(decoder, application1_.data.application.id))
		return die_s_("returned false", decoder);
	printf("OK\n");

	num_expected_ = 0;
	num_expected_other_chain_ = 3;
	if(is_ogg) { /* encoder moves vorbis comment after streaminfo according to ogg mapping. Also removes the seektable */
		expected_metadata_sequence_[num_expected_++] = &streaminfo_;
		expected_metadata_sequence_[num_expected_++] = &vorbiscomment_;
		expected_metadata_sequence_[num_expected_++] = &padding_;
		expected_metadata_sequence_[num_expected_++] = &application2_;
		expected_metadata_sequence_[num_expected_++] = &cuesheet_;
		expected_metadata_sequence_[num_expected_++] = &picture_;
		expected_metadata_sequence_[num_expected_++] = &unknown_;
	}
	else {
		expected_metadata_sequence_[num_expected_++] = &streaminfo_;
		expected_metadata_sequence_[num_expected_++] = &padding_;
		expected_metadata_sequence_[num_expected_++] = &seektable_;
		expected_metadata_sequence_[num_expected_++] = &application2_;
		expected_metadata_sequence_[num_expected_++] = &vorbiscomment_;
		expected_metadata_sequence_[num_expected_++] = &cuesheet_;
		expected_metadata_sequence_[num_expected_++] = &picture_;
		expected_metadata_sequence_[num_expected_++] = &unknown_;
	}

	if(!stream_decoder_test_respond_(decoder, &decoder_client_data, is_ogg, is_chained_ogg))
		return false;

	/*
	 * respond all, ignore APPLICATION id of app#1 & app#2
	 */

	printf("testing FLAC__stream_decoder_set_metadata_respond_all()... ");
	if(!FLAC__stream_decoder_set_metadata_respond_all(decoder))
		return die_s_("returned false", decoder);
	printf("OK\n");

	printf("testing FLAC__stream_decoder_set_metadata_ignore_application(of app block #1)... ");
	if(!FLAC__stream_decoder_set_metadata_ignore_application(decoder, application1_.data.application.id))
		return die_s_("returned false", decoder);
	printf("OK\n");

	printf("testing FLAC__stream_decoder_set_metadata_ignore_application(of app block #2)... ");
	if(!FLAC__stream_decoder_set_metadata_ignore_application(decoder, application2_.data.application.id))
		return die_s_("returned false", decoder);
	printf("OK\n");

	num_expected_ = 0;
	num_expected_other_chain_ = 3;
	if(is_ogg) { /* encoder moves vorbis comment after streaminfo according to ogg mapping. Also removes the seektable */
		expected_metadata_sequence_[num_expected_++] = &streaminfo_;
		expected_metadata_sequence_[num_expected_++] = &vorbiscomment_;
		expected_metadata_sequence_[num_expected_++] = &padding_;
		expected_metadata_sequence_[num_expected_++] = &cuesheet_;
		expected_metadata_sequence_[num_expected_++] = &picture_;
		expected_metadata_sequence_[num_expected_++] = &unknown_;
	}
	else {
		expected_metadata_sequence_[num_expected_++] = &streaminfo_;
		expected_metadata_sequence_[num_expected_++] = &padding_;
		expected_metadata_sequence_[num_expected_++] = &seektable_;
		expected_metadata_sequence_[num_expected_++] = &vorbiscomment_;
		expected_metadata_sequence_[num_expected_++] = &cuesheet_;
		expected_metadata_sequence_[num_expected_++] = &picture_;
		expected_metadata_sequence_[num_expected_++] = &unknown_;
	}

	if(!stream_decoder_test_respond_(decoder, &decoder_client_data, is_ogg, is_chained_ogg))
		return false;

	/*
	 * ignore all, respond VORBIS_COMMENT
	 */

	printf("testing FLAC__stream_decoder_set_metadata_ignore_all()... ");
	if(!FLAC__stream_decoder_set_metadata_ignore_all(decoder))
		return die_s_("returned false", decoder);
	printf("OK\n");

	printf("testing FLAC__stream_decoder_set_metadata_respond(VORBIS_COMMENT)... ");
	if(!FLAC__stream_decoder_set_metadata_respond(decoder, FLAC__METADATA_TYPE_VORBIS_COMMENT))
		return die_s_("returned false", decoder);
	printf("OK\n");

	num_expected_ = 0;
	num_expected_other_chain_ = 0;
	expected_metadata_sequence_[num_expected_++] = &vorbiscomment_;
	expected_metadata_sequence_other_chain_[num_expected_other_chain_++] = &vorbiscomment_;

	if(!stream_decoder_test_respond_(decoder, &decoder_client_data, is_ogg, is_chained_ogg))
		return false;

	/*
	 * ignore all, respond APPLICATION
	 */

	printf("testing FLAC__stream_decoder_set_metadata_ignore_all()... ");
	if(!FLAC__stream_decoder_set_metadata_ignore_all(decoder))
		return die_s_("returned false", decoder);
	printf("OK\n");

	printf("testing FLAC__stream_decoder_set_metadata_respond(APPLICATION)... ");
	if(!FLAC__stream_decoder_set_metadata_respond(decoder, FLAC__METADATA_TYPE_APPLICATION))
		return die_s_("returned false", decoder);
	printf("OK\n");

	num_expected_ = 0;
	num_expected_other_chain_ = 0;
	expected_metadata_sequence_[num_expected_++] = &application1_;
	expected_metadata_sequence_[num_expected_++] = &application2_;

	if(!stream_decoder_test_respond_(decoder, &decoder_client_data, is_ogg, is_chained_ogg))
		return false;

	/*
	 * ignore all, respond APPLICATION id of app#1
	 */

	printf("testing FLAC__stream_decoder_set_metadata_ignore_all()... ");
	if(!FLAC__stream_decoder_set_metadata_ignore_all(decoder))
		return die_s_("returned false", decoder);
	printf("OK\n");

	printf("testing FLAC__stream_decoder_set_metadata_respond_application(of app block #1)... ");
	if(!FLAC__stream_decoder_set_metadata_respond_application(decoder, application1_.data.application.id))
		return die_s_("returned false", decoder);
	printf("OK\n");

	num_expected_ = 0;
	num_expected_other_chain_ = 0;
	expected_metadata_sequence_[num_expected_++] = &application1_;

	if(!stream_decoder_test_respond_(decoder, &decoder_client_data, is_ogg, is_chained_ogg))
		return false;

	/*
	 * ignore all, respond APPLICATION id of app#1 & app#2
	 */

	printf("testing FLAC__stream_decoder_set_metadata_ignore_all()... ");
	if(!FLAC__stream_decoder_set_metadata_ignore_all(decoder))
		return die_s_("returned false", decoder);
	printf("OK\n");

	printf("testing FLAC__stream_decoder_set_metadata_respond_application(of app block #1)... ");
	if(!FLAC__stream_decoder_set_metadata_respond_application(decoder, application1_.data.application.id))
		return die_s_("returned false", decoder);
	printf("OK\n");

	printf("testing FLAC__stream_decoder_set_metadata_respond_application(of app block #2)... ");
	if(!FLAC__stream_decoder_set_metadata_respond_application(decoder, application2_.data.application.id))
		return die_s_("returned false", decoder);
	printf("OK\n");

	num_expected_ = 0;
	num_expected_other_chain_ = 0;
	expected_metadata_sequence_[num_expected_++] = &application1_;
	expected_metadata_sequence_[num_expected_++] = &application2_;

	if(!stream_decoder_test_respond_(decoder, &decoder_client_data, is_ogg, is_chained_ogg))
		return false;

	/*
	 * respond all, ignore APPLICATION, respond APPLICATION id of app#1
	 */

	printf("testing FLAC__stream_decoder_set_metadata_respond_all()... ");
	if(!FLAC__stream_decoder_set_metadata_respond_all(decoder))
		return die_s_("returned false", decoder);
	printf("OK\n");

	printf("testing FLAC__stream_decoder_set_metadata_ignore(APPLICATION)... ");
	if(!FLAC__stream_decoder_set_metadata_ignore(decoder, FLAC__METADATA_TYPE_APPLICATION))
		return die_s_("returned false", decoder);
	printf("OK\n");

	printf("testing FLAC__stream_decoder_set_metadata_respond_application(of app block #1)... ");
	if(!FLAC__stream_decoder_set_metadata_respond_application(decoder, application1_.data.application.id))
		return die_s_("returned false", decoder);
	printf("OK\n");

	num_expected_ = 0;
	num_expected_other_chain_ = 0;
	if(is_ogg) { /* encoder moves vorbis comment after streaminfo according to ogg mapping. Also removes the seektable */
		expected_metadata_sequence_[num_expected_++] = &streaminfo_;
		expected_metadata_sequence_[num_expected_++] = &vorbiscomment_;
		expected_metadata_sequence_[num_expected_++] = &padding_;
		expected_metadata_sequence_[num_expected_++] = &application1_;
		expected_metadata_sequence_[num_expected_++] = &cuesheet_;
		expected_metadata_sequence_[num_expected_++] = &picture_;
		expected_metadata_sequence_[num_expected_++] = &unknown_;

		expected_metadata_sequence_other_chain_[num_expected_other_chain_++] = &streaminfo_;
		expected_metadata_sequence_other_chain_[num_expected_other_chain_++] = &vorbiscomment_;
		expected_metadata_sequence_other_chain_[num_expected_other_chain_++] = &unknown_;
	}
	else {
		expected_metadata_sequence_[num_expected_++] = &streaminfo_;
		expected_metadata_sequence_[num_expected_++] = &padding_;
		expected_metadata_sequence_[num_expected_++] = &seektable_;
		expected_metadata_sequence_[num_expected_++] = &application1_;
		expected_metadata_sequence_[num_expected_++] = &vorbiscomment_;
		expected_metadata_sequence_[num_expected_++] = &cuesheet_;
		expected_metadata_sequence_[num_expected_++] = &picture_;
		expected_metadata_sequence_[num_expected_++] = &unknown_;
	}

	if(!stream_decoder_test_respond_(decoder, &decoder_client_data, is_ogg, is_chained_ogg))
		return false;

	/*
	 * ignore all, respond APPLICATION, ignore APPLICATION id of app#1
	 */

	printf("testing FLAC__stream_decoder_set_metadata_ignore_all()... ");
	if(!FLAC__stream_decoder_set_metadata_ignore_all(decoder))
		return die_s_("returned false", decoder);
	printf("OK\n");

	printf("testing FLAC__stream_decoder_set_metadata_respond(APPLICATION)... ");
	if(!FLAC__stream_decoder_set_metadata_respond(decoder, FLAC__METADATA_TYPE_APPLICATION))
		return die_s_("returned false", decoder);
	printf("OK\n");

	printf("testing FLAC__stream_decoder_set_metadata_ignore_application(of app block #1)... ");
	if(!FLAC__stream_decoder_set_metadata_ignore_application(decoder, application1_.data.application.id))
		return die_s_("returned false", decoder);
	printf("OK\n");

	num_expected_ = 0;
	num_expected_other_chain_ = 0;
	expected_metadata_sequence_[num_expected_++] = &application2_;

	if(!stream_decoder_test_respond_(decoder, &decoder_client_data, is_ogg, is_chained_ogg))
		return false;

	if(layer < LAYER_FILE) /* for LAYER_FILE, FLAC__stream_decoder_finish() closes the file */
		fclose(decoder_client_data.file);

	printf("testing FLAC__stream_decoder_delete()... ");
	FLAC__stream_decoder_delete(decoder);
	printf("OK\n");

	printf("\nPASSED!\n");

	return true;
}

FLAC__bool test_decoders(void)
{
	FLAC__bool is_ogg = false;
	FLAC__bool is_chained_ogg = false;
	samples_ = 1024 * 512;

	while(1) {
		init_metadata_blocks_();

		if(!generate_file_(is_ogg, is_chained_ogg))
			return false;

		if(!test_stream_decoder(LAYER_STREAM, is_ogg, is_chained_ogg))
			return false;

		if(!test_stream_decoder(LAYER_SEEKABLE_STREAM, is_ogg, is_chained_ogg))
			return false;

		if(!test_stream_decoder(LAYER_FILE, is_ogg, is_chained_ogg))
			return false;

		if(!test_stream_decoder(LAYER_FILENAME, is_ogg, is_chained_ogg))
			return false;

		(void) grabbag__file_remove_file(flacfilename(is_ogg, is_chained_ogg));

		free_metadata_blocks_();

		if(!FLAC_API_SUPPORTS_OGG_FLAC || is_chained_ogg)
			break;
		if(is_ogg)
			is_chained_ogg = true;
		is_ogg = true;
	}

	return true;
}
