/*
 *  jpeg_encode.cpp
 *  PHD Guiding
 *
 *  See jpeg_encode.h. libjpeg reports errors through error_exit, which by
 *  default calls exit(); here it longjmps back to EncodeGray8 instead. Every
 *  object with a destructor is made before the setjmp, so the longjmp skips
 *  none, and nothing on the way may throw through libjpeg's C frames: the
 *  destination's allocation failure becomes a libjpeg error.
 */

#include "jpeg_encode.h"

#include <csetjmp>
#include <cstdio>
#include <memory>
#include <vector>

#include <jpeglib.h>
#include <jerror.h>

namespace jpeg_encode
{

namespace
{

    struct ErrorManager
    {
        jpeg_error_mgr pub;
        std::jmp_buf jump;
        char message[JMSG_LENGTH_MAX];
    };

    void ErrorExit(j_common_ptr cinfo)
    {
        ErrorManager *err = reinterpret_cast<ErrorManager *>(cinfo->err);
        (*cinfo->err->format_message)(cinfo, err->message);
        std::longjmp(err->jump, 1);
    }

    // libjpeg's warnings and trace messages go to stderr by default; an encode
    // has none worth a line, and the daemon's stderr is the journal
    void OutputMessage(j_common_ptr) { }

    // Appends the compressed data to a std::string in 64 KiB blocks
    struct Destination
    {
        jpeg_destination_mgr pub;
        std::string *out;
        unsigned char block[65536];
    };

    void InitDestination(j_compress_ptr cinfo)
    {
        Destination *dest = reinterpret_cast<Destination *>(cinfo->dest);
        dest->pub.next_output_byte = dest->block;
        dest->pub.free_in_buffer = sizeof(dest->block);
    }

    bool Append(Destination *dest, size_t len)
    {
        try
        {
            dest->out->append(reinterpret_cast<const char *>(dest->block), len);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    boolean EmptyOutputBuffer(j_compress_ptr cinfo)
    {
        Destination *dest = reinterpret_cast<Destination *>(cinfo->dest);
        if (!Append(dest, sizeof(dest->block)))
            ERREXIT(cinfo, JERR_OUT_OF_MEMORY);
        dest->pub.next_output_byte = dest->block;
        dest->pub.free_in_buffer = sizeof(dest->block);
        return TRUE;
    }

    void TermDestination(j_compress_ptr cinfo)
    {
        Destination *dest = reinterpret_cast<Destination *>(cinfo->dest);
        if (!Append(dest, sizeof(dest->block) - dest->pub.free_in_buffer))
            ERREXIT(cinfo, JERR_OUT_OF_MEMORY);
    }

} // namespace

bool EncodeGray8(const unsigned char *gray, int width, int height, int quality, std::string *out, std::string *error)
{
    out->clear();

    // made before the setjmp: see the file comment
    std::vector<unsigned char> row(width > 0 ? (size_t) width * 3 : 0);
    std::unique_ptr<Destination> dest(new Destination);
    dest->out = out;

    jpeg_compress_struct cinfo;
    ErrorManager jerr;
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = ErrorExit;
    jerr.pub.output_message = OutputMessage;
    jerr.message[0] = '\0';

    if (setjmp(jerr.jump))
    {
        jpeg_destroy_compress(&cinfo);
        out->clear();
        if (error)
            *error = jerr.message;
        return false;
    }

    jpeg_create_compress(&cinfo);

    dest->pub.init_destination = InitDestination;
    dest->pub.empty_output_buffer = EmptyOutputBuffer;
    dest->pub.term_destination = TermDestination;
    cinfo.dest = &dest->pub;

    // what wxJPEGHandler::SaveFile sets, in its order
    cinfo.image_width = width > 0 ? width : 0;
    cinfo.image_height = height > 0 ? height : 0;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);

    jpeg_start_compress(&cinfo, TRUE);

    while (cinfo.next_scanline < cinfo.image_height)
    {
        const unsigned char *src = gray + (size_t) cinfo.next_scanline * cinfo.image_width;
        unsigned char *p = row.data();
        for (JDIMENSION x = 0; x < cinfo.image_width; x++)
        {
            *p++ = src[x];
            *p++ = src[x];
            *p++ = src[x];
        }
        JSAMPROW rowPointer = row.data();
        jpeg_write_scanlines(&cinfo, &rowPointer, 1);
    }

    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    return true;
}

} // namespace jpeg_encode
