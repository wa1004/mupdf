#include "mupdf/pdf/pdf_extension.h"
#include "../../thirdparty/zlib/zlib.h"

static const char *ntkoextobjname = "ntkoext";

char *new_time_string(fz_context *ctx) {
	int size = 15;
	char *timestring = fz_malloc(ctx, size);
	memset(timestring, 0, size);

	fz_lock(ctx, FZ_LOCK_FREETYPE);
	time_t t = time(0);
	struct tm *now = localtime(&t);
	fz_unlock(ctx, FZ_LOCK_FREETYPE);
	snprintf(timestring, size, "%04d%02d%02d%02d%02d%02d",
		now->tm_year + 1900, now->tm_mon + 1, now->tm_mday,
		now->tm_hour, now->tm_min, now->tm_sec);
	return timestring;
};

char *new_unique_string(fz_context *ctx, char *prefix, char *suffix) {
	char *tmp = new_time_string(ctx);
	int size = strlen(tmp) + (prefix ? strlen(prefix) : 0) + (suffix ? strlen(suffix) : 0);
	char *name = fz_malloc(ctx, size+1);
	memset(name, 0, size + 1);

	if (prefix) strcat(name, prefix);
	if (tmp) strcat(name, tmp);
	if (suffix) strcat(name, suffix);
	fz_free(ctx, tmp);
	return name;
}

fz_buffer *get_file_data(fz_context *ctx, char *filename, int ofs, int len) {
	fz_stream *stream = fz_open_file(ctx, filename);
	fz_buffer *buffer = fz_new_buffer(ctx, len);
	fz_seek(ctx, stream, ofs, 0);
	fz_read(ctx, stream, buffer->data, buffer->cap);
	buffer->len = buffer->cap;
	fz_drop_stream(ctx, stream);
	return buffer;
} 

ImageType img_recognize(char *filename) {
	char *ext = strrchr(filename, '.');
	if (!ext)
		return image_type_unkown;
	if (0 == fz_strcasecmp(ext, "jpeg") || 0 == fz_strcasecmp(ext, "jpg"))
		return image_type_jpg;
	if (0 == fz_strcasecmp(ext, "gif"))
		return image_type_gif;
	if (0 == fz_strcasecmp(ext, "png"))
		return image_type_gif;
	if (0 == fz_strcasecmp(ext, "gif"))
		return image_type_gif;
	return image_type_unkown;
}

int pdf_save_incremental_tofile(fz_context *ctx, pdf_document *doc,char *filename) {
	fz_lock(ctx, FZ_LOCK_FREETYPE);
	int ofs = fz_tell(ctx, doc->file);
	fz_seek(ctx, doc->file, 0, SEEK_SET);
	fz_buffer *buffer = fz_read_all(ctx, doc->file, doc->file_size);
	fz_save_buffer(ctx, buffer, filename);
	fz_seek(ctx, doc->file, ofs, SEEK_SET);
	fz_unlock(ctx, FZ_LOCK_FREETYPE);

	pdf_write_options opts = { 0 };
	opts.do_incremental = 1;
	pdf_save_document(ctx, doc, filename, &opts);
	return 0;
}

pdf_document * pdf_open_document_with_filename(fz_context * ctx, const char * filename, char * password)
{
	fz_stream *filestream = fz_open_file(ctx, filename);
	pdf_document *doc = pdf_open_document_with_filestream(ctx, filestream, password);
	return doc;
}

pdf_document * pdf_open_document_with_filestream(fz_context * ctx, fz_stream *file, char * password)
{
	pdf_document *doc = NULL;
	fz_try(ctx) {
		doc = pdf_open_document_with_stream(ctx, file);
		if (pdf_needs_password(ctx, doc))
		{
			int okay = pdf_authenticate_password(ctx, doc, password);
			if (!okay) {
				fz_throw(ctx, FZ_ERROR_GENERIC, "Can not anthenticate password");
			}
		}
	}
	fz_catch(ctx) {
		if( doc ) pdf_drop_document(ctx, doc);
		doc = NULL;
	}
	return doc;
}

fz_buffer * deflate_buffer_fromdata(fz_context *ctx, unsigned char *p, int n)
{
	fz_buffer *buf;
	int csize;
	int t;

	buf = fz_new_buffer(ctx, compressBound(n));
	csize = buf->cap;
	t = compress(buf->data, &csize, p, n);
	if (t != Z_OK)
	{
		fz_drop_buffer(ctx, buf);
		fz_throw(ctx, FZ_ERROR_GENERIC, "cannot deflate buffer");
	}
	buf->len = csize;
	return buf;
}
typedef void(*set_sample)(unsigned char*, unsigned char*);
static void 
set_sample_1(unsigned char *s, unsigned char *d) {
	d[2] = d[1] = d[0] = s[0];
}
static void 
set_sample_3(unsigned char *s, unsigned char *d) {
	d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
}

fz_buffer *fz_pixmap_rgb(fz_context *ctx, fz_pixmap *pixmap) {
	int pixcount = pixmap->w * pixmap->h;
	int imgsize = 3 * pixcount;
	fz_buffer *buffer = fz_new_buffer(ctx, imgsize);
	set_sample set_sample_fun = pixmap->n == 2 ? set_sample_1 : 
		(pixmap->n == 4 ? set_sample_3 : NULL);
	for (int i = 0; i < pixcount; i++) {
		int i_d = i * 3;
		int i_s = i * pixmap->n;
		if ((pixmap->samples + (i_s + pixmap->n - 1)) == 0)
			buffer->data[i_d] = buffer->data[i_d + 1] =
			buffer->data[i_d + 2] = 0;
		else
			set_sample_fun(pixmap->samples + i_s, buffer->data + i_d);
	}
	buffer->len = buffer->cap;
	return buffer;
}

fz_buffer *fz_pixmap_rgb_mask(fz_context *ctx, fz_pixmap *pixmap) {
	if (!ctx || !pixmap) return NULL;
	int pixCount = pixmap->h * pixmap->w;
	fz_buffer *buffer = fz_new_buffer(ctx, pixCount);
	for (int i = 0; i < pixCount; i++) {
		int startPos = i*pixmap->n;
		buffer->data[i] = 0x00;
		for (int n = 0; n < pixmap->n; n++) {
			if (pixmap->samples[startPos + n] != 0xff) {
				buffer->data[i] = 0xff; break;
			}
		}
	}
	buffer->len = buffer->cap;
	return buffer;
}

pdf_obj *add_image_xobj(fz_context *ctx, pdf_document *doc, Xobj_Image *xi)
{
	pdf_obj *xobj = pdf_new_dict(ctx, doc, 10);
	pdf_obj *subobj = pdf_new_name(ctx, doc, "XObject");
	pdf_dict_put_drop(ctx, xobj, PDF_NAME_Type, subobj);
	subobj = pdf_new_name(ctx, doc, "Image");
	pdf_dict_put_drop(ctx, xobj, PDF_NAME_Subtype, subobj);
	subobj = pdf_new_int(ctx, doc, xi->w);
	pdf_dict_put_drop(ctx, xobj, PDF_NAME_Width, subobj);
	subobj = pdf_new_int(ctx, doc, xi->h);
	pdf_dict_put_drop(ctx, xobj, PDF_NAME_Height, subobj);
	subobj = pdf_new_name(ctx, doc, xi->colorspace);
	pdf_dict_put_drop(ctx, xobj, PDF_NAME_ColorSpace, subobj);
	subobj = pdf_new_int(ctx, doc, xi->n);
	pdf_dict_put_drop(ctx, xobj, PDF_NAME_BitsPerComponent, subobj);
	subobj = pdf_new_name(ctx, doc, "FlateDecode");
	pdf_dict_put_drop(ctx, xobj, PDF_NAME_Filter, subobj);
	if (xi->maskobj) {
		pdf_dict_put(ctx, xobj, PDF_NAME_ImageMask, xi->maskobj);
	}
	xobj = pdf_add_object(ctx, doc, xobj);
	if (xi->data) {
		pdf_update_stream(ctx, doc, xobj, xi->data, 1);
	}
	return xobj;
}

pdf_obj *pdf_add_extstate(fz_context *ctx, pdf_document *doc) 
{
	pdf_obj *extobj = pdf_new_dict(ctx, doc, 5);
	pdf_dict_put_drop(ctx, extobj, PDF_NAME_Type, PDF_NAME_ExtGState);
	pdf_dict_put_drop(ctx, extobj, PDF_NAME_BM, pdf_new_name(ctx, doc, "Darken"));
	pdf_dict_put_drop(ctx, extobj, pdf_new_name(ctx, doc, "OP") , pdf_new_name(ctx, doc, "true"));
	pdf_dict_put_drop(ctx, extobj, pdf_new_name(ctx, doc, "AIS"), pdf_new_name(ctx, doc, "false"));
	pdf_dict_put_drop(ctx, extobj, PDF_NAME_ca, pdf_new_real(ctx, doc, 1.0));
	return pdf_add_object(ctx, doc, extobj);
} 

pdf_obj *pdf_add_pixmap(fz_context *ctx, pdf_document *doc, fz_pixmap *pixmap)
{
	if (!ctx || !doc || !pixmap) return NULL;
	fz_buffer *buffer = NULL;
	pdf_obj *maskobj = NULL;
	Xobj_Image xi = {pixmap->w, pixmap->h, 8, "FlateDecode", "DeviceGray", NULL, NULL};
#if 0 // looks like not need to add mask image!!!!
	buffer =	fz_pixmap_rgb_mask(ctx, pixmap);
	fz_buffer *bfCompressed = deflate_buffer_fromdata(ctx, buffer->data, buffer->len);
	xi.data = bfCompressed;
	fz_drop_buffer(ctx, buffer);
	pdf_obj *maskobj = add_image_xobj(ctx, doc, &xi);
	fz_drop_buffer(ctx, bfCompressed);
#endif 
	buffer = fz_pixmap_rgb(ctx, pixmap);
	fz_buffer *bfCompressed_rgb = deflate_buffer_fromdata(ctx, buffer->data, buffer->len);
	xi.colorspace = "DeviceRGB";
	xi.data = bfCompressed_rgb;
	xi.maskobj = maskobj;
	pdf_obj *imageobj = add_image_xobj(ctx, doc, &xi);
	fz_drop_buffer(ctx, bfCompressed_rgb);
	pdf_drop_obj(ctx, maskobj); 
	return imageobj;
}

pdf_obj *pdf_add_content(fz_context *ctx, pdf_document *doc, 
	char* xobjname, int x, int y, int w, int h)
{
	char bf[256] = { 0 };
	const char* fmt = "q /%s gs 1 0 0 1 0.00 0.00 cm\n"	\
		"%d 0 0 %d %d %d cm /%s Do Q";
	int size = snprintf(bf, 255, fmt, ntkoextobjname, w, h, x, y,
		xobjname);
	fz_buffer *buffer = fz_new_buffer(ctx, size);
	memcpy(buffer->data, bf, size);
	buffer->len = size;

	pdf_obj *obj = pdf_new_dict(ctx, doc, 1);
	pdf_obj *objRef = pdf_add_object(ctx, doc, obj);
	pdf_update_stream(ctx, doc, obj, buffer, 0);

	fz_drop_buffer(ctx, buffer);
	return objRef;
}

int pdf_page_add_content(fz_context *ctx, pdf_document *doc, pdf_obj *page, pdf_obj *objref)
{
	pdf_obj *contentsobj = pdf_dict_gets(ctx, page, "Contents");
	if (!pdf_is_array(ctx, contentsobj)) {
		pdf_keep_obj(ctx, contentsobj);
		pdf_dict_dels(ctx, page, "Contents");

		pdf_obj *arrobj = pdf_new_array(ctx, doc, 2);
		pdf_array_push_drop(ctx, arrobj, contentsobj);
		contentsobj = arrobj;
		pdf_dict_put_drop(ctx, page, PDF_NAME_Contents, contentsobj);
	}
	pdf_array_push_drop(ctx, contentsobj, objref);
	return 0;
}

int pdf_resource_add_xobj(fz_context *ctx, pdf_document *doc, pdf_obj *resobj,
	char *name, pdf_obj *ref)
{
	pdf_obj *xobj = pdf_dict_gets(ctx, resobj, "XObject");
	if (!xobj) {
		xobj = pdf_new_dict(ctx, doc, 4);
		pdf_dict_put_drop(ctx, resobj, PDF_NAME_XObject, xobj);
	}
	pdf_obj *nameobj = pdf_new_name(ctx, doc, name);
	pdf_dict_put_drop(ctx, xobj, nameobj, ref);
	pdf_drop_obj(ctx, nameobj);
	return 0;
}

int pdf_resource_add_extgstate(fz_context *ctx, pdf_document *doc, pdf_obj *resobj,
	const char *name, pdf_obj *ref) 
{
	pdf_obj *egsObj = pdf_dict_gets(ctx, resobj, "ExtGState");
	if (!egsObj) {
		egsObj = pdf_new_dict(ctx, doc, 1);
		pdf_dict_put_drop(ctx, resobj, PDF_NAME_ExtGState, egsObj);
		pdf_add_object(ctx, doc, egsObj);
	}
	pdf_dict_put_drop(ctx, egsObj, pdf_new_name(ctx, doc, ntkoextobjname), ref);
	return 0;
}

int pdf_add_image_with_document(fz_context *ctx, pdf_document *doc, fz_buffer*imgbf,
	int pageno, int x, int y, int w, int h)
{
	pdf_obj * pageobj = pdf_lookup_page_obj(ctx, doc, pageno);
	fz_image *fzimage = fz_new_image_from_buffer(ctx, imgbf);
	fz_pixmap *image = fz_get_pixmap_from_image(ctx, fzimage, NULL, NULL, NULL, NULL);
	fz_drop_image(ctx, fzimage);
	pdf_obj * xobjRef = pdf_add_pixmap(ctx, doc, image);
	fz_drop_pixmap(ctx, image);

	pdf_obj * egsObjRef = pdf_add_extstate(ctx, doc);
	pdf_obj *resobj = pdf_dict_gets(ctx, pageobj, "Resources");
	pdf_resource_add_extgstate(ctx, doc, resobj, ntkoextobjname, egsObjRef);
	unsigned char *xobjname = new_unique_string(ctx, "ntkoimage", NULL);
	pdf_resource_add_xobj(ctx, doc, resobj, xobjname, xobjRef);
	pdf_obj *contentobj = pdf_add_content(ctx, doc, xobjname, x, y, w, h);
	fz_free(ctx, xobjname);
	pdf_page_add_content(ctx, doc, pageobj, contentobj);

	return 0;
}

int pdf_add_image_with_filestream(fz_context *ctx, fz_stream*file, fz_buffer*imgbf,
	char *outfile, int pageno, int x, int y, int w, int h, char *password)
{
	pdf_document *doc = pdf_open_document_with_filestream(ctx, file,password);
	int pagecount = pdf_count_pages(ctx, doc);
	if (pageno < 0) pageno = 0;
	if (pageno >= pagecount) {
		fz_throw(ctx, 1, "page number ot of range");
		return 1;
	}
	int code = pdf_add_image_with_document(ctx, doc, imgbf, pageno, x, y, w, h);
	if( !code ) 
		pdf_save_incremental_tofile(ctx, doc, outfile);
	pdf_drop_document(ctx, doc);
	return code;
}

int pdf_add_image_with_filename(fz_context *ctx, char *pdffile, char *imgfile,
	char *outfile, int pageno, int x, int y, int w, int h, char *password)
{
	fz_stream *file = fz_open_file(ctx, pdffile);
	fz_buffer *img = fz_read_file(ctx, imgfile);
	int code = pdf_add_image_with_filestream(ctx, file, img, outfile, pageno, x, y, w, h,password);
	fz_drop_stream(ctx, file);
	fz_drop_buffer(ctx, img);
	return code;
}

// stderr fileno is 2!!
static FILE *stderr_new = NULL;
static int oldfd = 0;
void stderr_tofile(char *filename)
{
	if (oldfd != 0) return;
	oldfd = _dup(2);
	stderr_new = freopen(filename, "a", stderr);
}

void stderr_restore() {
	if (NULL == stderr_new) return;
	fflush(stderr_new); stderr_new = NULL;
	_dup2(oldfd, 2);
}

fz_rect pdf_page_box(fz_context *ctx, pdf_document *doc, int pageno)
{
	pdf_obj *pageobj = pdf_lookup_page_obj(ctx, doc, pageno);
    if(!pageobj) return fz_empty_rect;
	fz_rect bbox = fz_empty_irect;

	fz_try(ctx)
	{
		obj = pdf_dict_get(ctx, page,PDF_NAME_MediaBox);
		if (!pdf_is_array(ctx, obj))
			break;
		pdf_to_rect(ctx, obj, &bbox);
	}
	fz_catch(ctx)
	{
	}
	return bbox;
}
