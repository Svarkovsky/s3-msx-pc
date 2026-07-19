/**
 * ds-stlz.c — STLZ Public Functions Implementation
 * 
 * 
 * Implements the lightweight file-format inspection functions
 * for the Striped Delta-Stride LZ (STLZ) image codec.
 * 
 * These functions have zero dependency on Retro-Go internals
 * and compile independently. All heavy codec logic remains
 * in the header as static inline functions.
 * 
 * Copyright (C) 2026 Ivan Svarkovsky <ivansvarkovsky@gmail.com>
 * 
 * Distributed under CC BY-NC-SA 4.0 license.
 * See ds-stlz.h for full license text.
 * 
 * Original repository: https://github.com/Svarkovsky
 */


/*
 * THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "ds-stlz.h"

bool is_stlz_file(const char *filepath) {
    if (!filepath || !filepath[0]) return false;
    
    FILE *f = fopen(filepath, "rb");
    if (!f) return false;
    
    char magic[4];
    size_t r = fread(magic, 1, 4, f);
    fclose(f);
    
    return (r == 4 && memcmp(magic, STLZ_MAGIC, 4) == 0);
}

bool rg_stlz_get_dimensions(const char *filepath, int *width, int *height) {
    FILE *f = fopen(filepath, "rb");
    if (!f) return false;
    
    stlz_header_t hdr;
    if (fread(&hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        fclose(f);
        return false;
    }
    fclose(f);
    
    if (memcmp(hdr.magic, STLZ_MAGIC, 4) != 0) return false;
    
    if (width) *width = hdr.width;
    if (height) *height = hdr.height;
    return true;
}
