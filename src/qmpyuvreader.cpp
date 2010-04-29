/*
 *  qmpwidget - A Qt widget for embedding MPlayer
 *  Copyright (C) 2010 by Jonas Gehring
 * 	All rights reserved.
 *
 * 	Redistribution and use in source and binary forms, with or without
 * 	modification, are permitted provided that the following conditions are met:
 *      * Redistributions of source code must retain the above copyright
 * 	      notice, this list of conditions and the following disclaimer.
 * 	    * Redistributions in binary form must reproduce the above copyright
 * 	      notice, this list of conditions and the following disclaimer in the
 * 	      documentation and/or other materials provided with the distribution.
 * 	    * Neither the name of the copyright holders nor the
 * 	      names of its contributors may be used to endorse or promote products
 * 	      derived from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 */


#include <QImage>
#include <QMutex>
#include <QThread>

#include <cstdio>


// Internal YUV pipe reader
class QMPYuvReader : public QThread
{
	Q_OBJECT

	public:
		// Constructor
		QMPYuvReader(QString pipe, QObject *parent = 0)
			: QThread(parent), m_pipe(pipe), m_saveme(NULL), m_savemeSize(-1)
		{
			initTables();
		}

		// Destructor
		~QMPYuvReader()
		{
			delete[] m_saveme;
		}

		// Tells the thread to stop and exit
		void stop()
		{
			m_mutex.lock();
			m_stop = true;
			m_mutex.unlock();
			wait();
		}

	protected:
		// Main thread loop
		void run()
		{
			FILE *f = fopen(m_pipe.toLocal8Bit().data(), "rb");
			if (f == NULL) {
				return;
			}

			// Parse stream header
			char c;
			int width, height, fps, t1, t2;
			int n = fscanf(f, "YUV4MPEG2 W%d H%d F%d:1 I%c A%d:%d", &width, &height, &fps, &c, &t1, &t2);
			if (n < 3) {
				fclose(f);
				return;
			}

			unsigned char *yuv[3];
			yuv[0] = new unsigned char[width * height];
			yuv[1] = new unsigned char[width * height];
			yuv[2] = new unsigned char[width * height];

			QImage image(width, height, QImage::Format_ARGB32);

			// Read frames
			const int ysize = width * height;
			const int csize = width * height / 4;
			while (true) {
				m_mutex.lock();
				if (m_stop) {
					m_mutex.unlock();
					break;
				}
				m_mutex.unlock();

				fread(yuv[0], 1, 6, f);
				fread(yuv[0], 1, ysize, f);
				fread(yuv[1], 1, csize, f);
				fread(yuv[2], 1, csize, f);
				supersample(yuv[1], width, height);
				supersample(yuv[2], width, height);
				yuvToQImage(yuv, &image, width, height);

				emit imageReady(image);
			}

			delete[] yuv[0];
			delete[] yuv[1];
			delete[] yuv[2];
			fclose(f);
		}

		// 420 to 444 supersampling (from mjpegtools)
		void supersample(unsigned char *buffer, int width, int height)
		{
			unsigned char *inm, *in0, *inp, *out0, *out1;
			unsigned char cmm, cm0, cmp, c0m, c00, c0p, cpm, cp0, cpp;
			int x, y;

			if (m_saveme == NULL || width > m_savemeSize) {
				delete[] m_saveme;
				m_savemeSize = width;
				m_saveme = new unsigned char[m_savemeSize];
			}
			memcpy(m_saveme, buffer, width);

			in0 = buffer + (width * height / 4) - 2;
			inm = in0 - width/2;
			inp = in0 + width/2;
			out1 = buffer + (width * height) - 1;
			out0 = out1 - width;

			for (y = height; y > 0; y -= 2) {
				if (y == 2) {
					in0 = m_saveme + width/2 - 2;
					inp = in0 + width/2;
				}
				for (x = width; x > 0; x -= 2) {
					cmm = ((x == 2) || (y == 2)) ? in0[1] : inm[0];
					cm0 = (y == 2) ? in0[1] : inm[1];
					cmp = ((x == width) || (y == 2)) ? in0[1] : inm[2];
					c0m = (x == 2) ? in0[1] : in0[0];
					c00 = in0[1];
					c0p = (x == width) ? in0[1] : in0[2];
					cpm = ((x == 2) || (y == height)) ? in0[1] : inp[0];
					cp0 = (y == height) ? in0[1] : inp[1];
					cpp = ((x == width) || (y == height)) ? in0[1] : inp[2];
					inm--;
					in0--;
					inp--;

					*(out1--) = (1*cpp + 3*(cp0+c0p) + 9*c00 + 8) >> 4;
					*(out1--) = (1*cpm + 3*(cp0+c0m) + 9*c00 + 8) >> 4;
					*(out0--) = (1*cmp + 3*(cm0+c0p) + 9*c00 + 8) >> 4;
					*(out0--) = (1*cmm + 3*(cm0+c0m) + 9*c00 + 8) >> 4;
				}
				out1 -= width;
				out0 -= width;
			}
		}

		// Converts YCbCr data to a QImage
		void yuvToQImage(unsigned char *planes[], QImage *dest, int width, int height)
		{
			unsigned char *yptr = planes[0];
			unsigned char *cbptr = planes[1];
			unsigned char *crptr = planes[2];

			// This is partly from mjpegtools
			for (int y = 0; y < height; y++) {
				QRgb *dptr = (QRgb *)dest->scanLine(y);
				for (int x = 0; x < width; x++) {
					*dptr = qRgb(qBound(0, (RGB_Y[*yptr] + R_Cr[*crptr]) >> 18, 255),
						qBound(0, (RGB_Y[*yptr] + G_Cb[*cbptr]+ G_Cr[*crptr]) >> 18, 255),
						qBound(0, (RGB_Y[*yptr] + B_Cb[*cbptr]) >> 18, 255));
					++yptr;
					++cbptr;
					++crptr;
					++dptr;
				}
			}
		}

		// Rounding towards zero
		inline int zround(double n)
		{
			if (n >= 0) {
				return (int)(n + 0.5);
			} else {
				return (int)(n - 0.5);
			}
		}

		// Initializes the YCbCr -> RGB conversion tables (again, from mjpegtools)
		void initTables(void)
		{
			/* clip Y values under 16 */
			for (int i = 0; i < 16; i++) {
				RGB_Y[i] = zround((1.0 * (double)(16 - 16) * 255.0 / 219.0 * (double)(1<<18)) + (double)(1<<(18-1)));
			}
			for (int i = 16; i < 236; i++) {
				RGB_Y[i] = zround((1.0 * (double)(i - 16) * 255.0 / 219.0 * (double)(1<<18)) + (double)(1<<(18-1)));
			}
			/* clip Y values above 235 */
			for (int i = 236; i < 256; i++) {
				RGB_Y[i] = zround((1.0 * (double)(235 - 16)  * 255.0 / 219.0 * (double)(1<<18)) + (double)(1<<(18-1)));
			}

			/* clip Cb/Cr values below 16 */   
			for (int i = 0; i < 16; i++) {
				R_Cr[i] = zround(1.402 * (double)(-112) * 255.0 / 224.0 * (double)(1<<18));
				G_Cr[i] = zround(-0.714136 * (double)(-112) * 255.0 / 224.0 * (double)(1<<18));
				G_Cb[i] = zround(-0.344136 * (double)(-112) * 255.0 / 224.0 * (double)(1<<18));
				B_Cb[i] = zround(1.772 * (double)(-112) * 255.0 / 224.0 * (double)(1<<18));
			}
			for (int i = 16; i < 241; i++) {
				R_Cr[i] = zround(1.402 * (double)(i - 128) * 255.0 / 224.0 * (double)(1<<18));
				G_Cr[i] = zround(-0.714136 * (double)(i - 128) * 255.0 / 224.0 * (double)(1<<18));
				G_Cb[i] = zround(-0.344136 * (double)(i - 128) * 255.0 / 224.0 * (double)(1<<18));
				B_Cb[i] = zround(1.772 * (double)(i - 128) * 255.0 / 224.0 * (double)(1<<18));
			}
			/* clip Cb/Cr values above 240 */  
			for (int i = 241; i < 256; i++) {
				R_Cr[i] = zround(1.402 * (double)(112) * 255.0 / 224.0 * (double)(1<<18));
				G_Cr[i] = zround(-0.714136 * (double)(112) * 255.0 / 224.0 * (double)(1<<18));
				G_Cb[i] = zround(-0.344136 * (double)(i - 128) * 255.0 / 224.0 * (double)(1<<18));
				B_Cb[i] = zround(1.772 * (double)(112) * 255.0 / 224.0 * (double)(1<<18));
			}
		}

	signals:
		void imageReady(const QImage &image);

	private:
		QString m_pipe;
		QMutex m_mutex;
		bool m_stop;

		// Conversion tables
		int RGB_Y[256];
		int R_Cr[256];
		int G_Cb[256];
		int G_Cr[256];
		int B_Cb[256];

		// Temporary buffers
		unsigned char *m_saveme;
		int m_savemeSize;
};


#include "qmpyuvreader.moc"