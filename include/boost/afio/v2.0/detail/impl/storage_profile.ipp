/* storage_profile.hpp
A profile of an OS and filing system
(C) 2015 Niall Douglas http://www.nedprod.com/
File Created: Dec 2015


Boost Software License - Version 1.0 - August 17th, 2003

Permission is hereby granted, free of charge, to any person or organization
obtaining a copy of the software and accompanying documentation covered by
this license (the "Software") to use, reproduce, display, distribute,
execute, and transmit the Software, and to prepare derivative works of the
Software, and to permit third-parties to whom the Software is furnished to
do so, all subject to the following:

The copyright notices in the Software and this entire statement, including
the above license grant, this restriction and the following disclaimer,
must be included in all copies of the Software, in whole or in part, and
all derivative works of the Software, unless such copies or derivative
works are solely in the form of machine-executable object code generated by
a source language processor.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.
*/

#include "../../file_handle.hpp"
#include "../../statfs.hpp"
#include "../../storage_profile.hpp"
#include "../../utils.hpp"

#include <vector>
#ifndef NDEBUG
#include <iostream>
#endif

#define BOOST_AFIO_STORAGE_PROFILE_TIME_DIVIDER 10

BOOST_AFIO_V2_NAMESPACE_BEGIN

namespace storage_profile
{

  /* YAML's syntax is amazingly powerful ... we can express a map
  of a map to a map using this syntax:

  ?
  direct: 0
  sync: 0
  :
  concurrency:
  atomicity:
  min_atomic_write: 1
  max_atomic_write: 1

  Some YAML parsers appear to accept this more terse form too:

  {direct: 0, sync: 0}:
  concurrency:
  atomicity:
  min_atomic_write: 1
  max_atomic_write: 1

  We don't do any of this as some YAML parsers are basically JSON parsers with
  some rules relaxed. We just use:

  direct=0 sync=0:
  concurrency:
  atomicity:
  min_atomic_write: 1
  max_atomic_write: 1
  */
  void storage_profile::write(std::ostream &out, std::regex which, size_t _indent, bool invert_match) const
  {
    BOOST_AFIO_LOG_FUNCTION_CALL(this);
    std::vector<std::string> lastsection;
    auto print = [_indent, &out, &lastsection](auto &i) {
      size_t indent = _indent;
      if(i.value != default_value<decltype(i.value)>())
      {
        std::vector<std::string> thissection;
        const char *s, *e;
        for(s = i.name, e = i.name; *e; e++)
        {
          if(*e == ':')
          {
            thissection.push_back(std::string(s, e - s));
            s = e + 1;
          }
        }
        std::string name(s, e - s);
        for(size_t n = 0; n < thissection.size(); n++)
        {
          indent += 4;
          if(n >= lastsection.size() || thissection[n] != lastsection[n])
          {
            out << std::string(indent - 4, ' ') << thissection[n] << ":\n";
          }
        }
        if(i.description)
        {
          std::string text(i.description);
          std::vector<std::string> lines;
          for(;;)
          {
            size_t idx = 78;
            if(idx < text.size())
            {
              while(text[idx] != ' ')
                --idx;
            }
            else
              idx = text.size();
            lines.push_back(text.substr(0, idx));
            if(idx < text.size())
              text = text.substr(idx + 1);
            else
              break;
          }
          for(auto &line : lines)
            out << std::string(indent, ' ') << "# " << line << "\n";
        }
        out << std::string(indent, ' ') << name << ": " << i.value << "\n";
        if(i.description && strlen(i.description) > 78)
          out << "\n";
        lastsection = std::move(thissection);
      }
    };
    for(const item_erased &i : *this)
    {
      bool matches = std::regex_match(i.name, which);
      if((matches && !invert_match) || (!matches && invert_match))
        i.invoke(print);
    }
  }

  namespace system
  {
    namespace detail
    {
      // From http://burtleburtle.net/bob/rand/smallprng.html
      typedef unsigned int u4;
      typedef struct ranctx
      {
        u4 a;
        u4 b;
        u4 c;
        u4 d;
      } ranctx;

#define rot(x, k) (((x) << (k)) | ((x) >> (32 - (k))))
      static u4 ranval(ranctx *x)
      {
        u4 e = x->a - rot(x->b, 27);
        x->a = x->b ^ rot(x->c, 17);
        x->b = x->c + x->d;
        x->c = x->d + e;
        x->d = e + x->a;
        return x->d;
      }
#undef rot

      static void raninit(ranctx *x, u4 seed)
      {
        u4 i;
        x->a = 0xf1ea5eed, x->b = x->c = x->d = seed;
        for(i = 0; i < 20; ++i)
        {
          (void) ranval(x);
        }
      }
    }

    // System memory quantity, in use, max and min bandwidth
    outcome<void> mem(storage_profile &sp, file_handle &h) noexcept
    {
      static unsigned long long mem_quantity, mem_max_bandwidth, mem_min_bandwidth;
      static float mem_in_use;
      if(mem_quantity)
      {
        sp.mem_quantity.value = mem_quantity;
        sp.mem_in_use.value = mem_in_use;
        sp.mem_max_bandwidth.value = mem_max_bandwidth;
        sp.mem_min_bandwidth.value = mem_min_bandwidth;
      }
      else
      {
        try
        {
          size_t chunksize = 256 * 1024 * 1024;
#ifdef WIN32
          BOOST_OUTCOME_TRYV(windows::_mem(sp, h));
#else
          BOOST_OUTCOME_TRYV(posix::_mem(sp, h));
#endif

          if(sp.mem_quantity.value / 4 < chunksize)
            chunksize = (size_t)(sp.mem_quantity.value / 4);
          char *buffer = utils::page_allocator<char>().allocate(chunksize);
          auto unbuffer = BOOST_AFIO_V2_NAMESPACE::undoer([buffer, chunksize] { utils::page_allocator<char>().deallocate(buffer, chunksize); });
          // Make sure all memory is really allocated first
          memset(buffer, 1, chunksize);

          // Max bandwidth is sequential writes of min(25% of system memory or 256Mb)
          auto begin = stl11::chrono::high_resolution_clock::now();
          unsigned long long count;
          for(count = 0; stl11::chrono::duration_cast<stl11::chrono::seconds>(stl11::chrono::high_resolution_clock::now() - begin).count() < (10 / BOOST_AFIO_STORAGE_PROFILE_TIME_DIVIDER); count++)
          {
            memset(buffer, count & 0xff, chunksize);
          }
          sp.mem_max_bandwidth.value = (unsigned long long) ((double) count * chunksize / 10);

          // Min bandwidth is randomised 4Kb copies of the same
          detail::ranctx ctx;
          detail::raninit(&ctx, 78);
          begin = stl11::chrono::high_resolution_clock::now();
          for(count = 0; stl11::chrono::duration_cast<stl11::chrono::seconds>(stl11::chrono::high_resolution_clock::now() - begin).count() < (10 / BOOST_AFIO_STORAGE_PROFILE_TIME_DIVIDER); count++)
          {
            for(size_t n = 0; n < chunksize; n += 4096)
            {
              auto offset = detail::ranval(&ctx) * 4096;
              offset = offset % chunksize;
              memset(buffer + offset, count & 0xff, 4096);
            }
          }
          sp.mem_min_bandwidth.value = (unsigned long long) ((double) count * chunksize / 10);
        }
        catch(...)
        {
          return std::current_exception();
        }
        mem_quantity = sp.mem_quantity.value;
        mem_in_use = sp.mem_in_use.value;
        mem_max_bandwidth = sp.mem_max_bandwidth.value;
        mem_min_bandwidth = sp.mem_min_bandwidth.value;
      }
      return make_valued_outcome<void>();
    }
  }
  namespace storage
  {
    // Device name, size, min i/o size
    outcome<void> device(storage_profile &sp, file_handle &h) noexcept
    {
      try
      {
        statfs_t fsinfo;
        BOOST_OUTCOME_TRYV(fsinfo.fill(h, statfs_t::want::iosize | statfs_t::want::mntfromname | statfs_t::want::fstypename));
        sp.device_min_io_size.value = (unsigned) fsinfo.f_iosize;
#ifdef WIN32
        BOOST_OUTCOME_TRYV(windows::_device(sp, h, fsinfo.f_mntfromname, fsinfo.f_fstypename));
#else
        BOOST_OUTCOME_TRYV(posix::_device(sp, h, fsinfo.f_mntfromname, fsinfo.f_fstypename));
#endif
      }
      catch(...)
      {
        return std::current_exception();
      }
      return make_valued_outcome<void>();
    }
    // FS name, config, size, in use
    outcome<void> fs(storage_profile &sp, file_handle &h) noexcept
    {
      try
      {
        statfs_t fsinfo;
        BOOST_OUTCOME_TRYV(fsinfo.fill(h));
        sp.fs_name.value = fsinfo.f_fstypename;
        sp.fs_config.value = "todo";
        sp.fs_size.value = fsinfo.f_blocks * fsinfo.f_bsize;
        sp.fs_in_use.value = (float) (fsinfo.f_blocks - fsinfo.f_bfree) / fsinfo.f_blocks;
      }
      catch(...)
      {
        return std::current_exception();
      }
      return make_valued_outcome<void>();
    }
  }

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4459)  // off_t shadows global namespace
#endif
  namespace concurrency
  {
    outcome<void> atomic_rewrite_quantum(storage_profile &sp, file_handle &srch) noexcept
    {
      try
      {
        using off_t = io_service::extent_type;
        sp.max_aligned_atomic_rewrite.value = 1;
        sp.atomic_rewrite_quantum.value = (off_t) -1;
        for(size_t size = srch.requires_aligned_io() ? 512 : 64; size <= 1 * 1024 * 1024 && size < sp.atomic_rewrite_quantum.value; size = size * 2)
        {
          // Create two concurrent writer threads and as many reader threads as additional CPU cores
          std::vector<std::thread> writers, readers;
          std::atomic<size_t> done(2);
          for(char no = '1'; no <= '2'; no++)
            writers.push_back(std::thread([size, &srch, no, &done] {
              auto _h(srch.clone());
              if(!_h)
                throw std::runtime_error("concurrency::atomic_rewrite_quantum: Could not open work file due to " + _h.get_error().message());
              file_handle h(std::move(_h.get()));
              std::vector<char> buffer(size, no);
              file_handle::const_buffer_type _reqs[1] = {std::make_pair(buffer.data(), size)};
              file_handle::io_request<file_handle::const_buffers_type> reqs(_reqs, 0);
              --done;
              while(done)
                std::this_thread::yield();
              while(!done)
              {
                h.write(reqs);
              }
            }));
          // Wait till the writers launch
          while(done)
            std::this_thread::yield();
          unsigned concurrency = std::thread::hardware_concurrency() - 2;
          if(concurrency < 4)
            concurrency = 4;
          std::atomic<io_service::extent_type> atomic_rewrite_quantum(sp.atomic_rewrite_quantum.value);
          std::atomic<bool> failed(false);
          for(unsigned no = 0; no < concurrency; no++)
            readers.push_back(std::thread([size, &srch, no, &done, &atomic_rewrite_quantum, &failed] {
              auto _h(srch.clone());
              if(!_h)
                throw std::runtime_error("concurrency::atomic_rewrite_quantum: Could not open work file due to " + _h.get_error().message());
              file_handle h(std::move(_h.get()));
              std::vector<char> buffer(size, 0), tocmp(size, 0);
              file_handle::buffer_type _reqs[1] = {std::make_pair(buffer.data(), size)};
              file_handle::io_request<file_handle::buffers_type> reqs(_reqs, 0);
              while(!done)
              {
                h.read(reqs);
                // memset(tocmp.data(), buffer.front(), size);
                // if (memcmp(buffer.data(), tocmp.data(), size))
                {
                  const size_t *data = (size_t *) buffer.data(), *end = (size_t *) (buffer.data() + size);
                  for(const size_t *d = data; d < end; d++)
                  {
                    if(*d != *data)
                    {
                      failed = true;
                      off_t failedat = d - data;
                      if(failedat < atomic_rewrite_quantum)
                      {
#ifndef NDEBUG
                        std::cout << "  Torn rewrite at offset " << failedat << std::endl;
#endif
                        atomic_rewrite_quantum = failedat;
                      }
                      break;
                    }
                  }
                }
              }
            }));

#ifndef NDEBUG
          std::cout << "direct=" << !srch.are_reads_from_cache() << " sync=" << srch.are_writes_durable() << " testing atomicity of rewrites of " << size << " bytes ..." << std::endl;
#endif
          auto begin = stl11::chrono::high_resolution_clock::now();
          while(!failed && stl11::chrono::duration_cast<stl11::chrono::seconds>(stl11::chrono::high_resolution_clock::now() - begin).count() < (20 / BOOST_AFIO_STORAGE_PROFILE_TIME_DIVIDER))
          {
            stl11::this_thread::sleep_for(stl11::chrono::seconds(1));
          }
          done = true;
          for(auto &writer : writers)
            writer.join();
          for(auto &reader : readers)
            reader.join();
          sp.atomic_rewrite_quantum.value = atomic_rewrite_quantum;
          if(!failed)
          {
            if(size > sp.max_aligned_atomic_rewrite.value)
              sp.max_aligned_atomic_rewrite.value = size;
          }
          else
            break;
        }
        if(sp.atomic_rewrite_quantum.value > sp.max_aligned_atomic_rewrite.value)
          sp.atomic_rewrite_quantum.value = sp.max_aligned_atomic_rewrite.value;

        // If burst quantum exceeds rewrite quantum, make sure it does so at
        // offsets not at the front of the file
        if(sp.max_aligned_atomic_rewrite.value > sp.atomic_rewrite_quantum.value)
        {
          size_t size = (size_t) sp.max_aligned_atomic_rewrite.value;
          for(off_t offset = sp.max_aligned_atomic_rewrite.value; offset < sp.max_aligned_atomic_rewrite.value * 4; offset += sp.max_aligned_atomic_rewrite.value)
          {
            // Create two concurrent writer threads and as many reader threads as additional CPU cores
            std::vector<std::thread> writers, readers;
            std::atomic<size_t> done(2);
            for(char no = '1'; no <= '2'; no++)
              writers.push_back(std::thread([size, offset, &srch, no, &done] {
                auto _h(srch.clone());
                if(!_h)
                  throw std::runtime_error("concurrency::atomic_rewrite_quantum: Could not open work file due to " + _h.get_error().message());
                file_handle h(std::move(_h.get()));
                std::vector<char> buffer(size, no);
                file_handle::const_buffer_type _reqs[1] = {std::make_pair(buffer.data(), size)};
                file_handle::io_request<file_handle::const_buffers_type> reqs(_reqs, offset);
                --done;
                while(done)
                  std::this_thread::yield();
                while(!done)
                {
                  h.write(reqs);
                }
              }));
            // Wait till the writers launch
            while(done)
              std::this_thread::yield();
            unsigned concurrency = std::thread::hardware_concurrency() - 2;
            if(concurrency < 4)
              concurrency = 4;
            std::atomic<io_service::extent_type> max_aligned_atomic_rewrite(sp.max_aligned_atomic_rewrite.value);
            std::atomic<bool> failed(false);
            for(unsigned no = 0; no < concurrency; no++)
              readers.push_back(std::thread([size, offset, &srch, no, &done, &max_aligned_atomic_rewrite, &failed] {
                auto _h(srch.clone());
                if(!_h)
                  throw std::runtime_error("concurrency::atomic_rewrite_quantum: Could not open work file due to " + _h.get_error().message());
                file_handle h(std::move(_h.get()));
                std::vector<char> buffer(size, 0), tocmp(size, 0);
                file_handle::buffer_type _reqs[1] = {std::make_pair(buffer.data(), size)};
                file_handle::io_request<file_handle::buffers_type> reqs(_reqs, offset);
                while(!done)
                {
                  h.read(reqs);
                  // memset(tocmp.data(), buffer.front(), size);
                  // if (memcmp(buffer.data(), tocmp.data(), size))
                  {
                    const size_t *data = (size_t *) buffer.data(), *end = (size_t *) (buffer.data() + size);
                    for(const size_t *d = data; d < end; d++)
                    {
                      if(*d != *data)
                      {
                        failed = true;
                        off_t failedat = (d - data);
                        if(failedat < max_aligned_atomic_rewrite)
                        {
#ifndef NDEBUG
                          std::cout << "  Torn rewrite at offset " << failedat << std::endl;
#endif
                          max_aligned_atomic_rewrite = failedat;
                        }
                        break;
                      }
                    }
                  }
                }
              }));

#ifndef NDEBUG
            std::cout << "direct=" << !srch.are_reads_from_cache() << " sync=" << srch.are_writes_durable() << " testing atomicity of rewrites of " << size << " bytes to offset " << offset << " ..." << std::endl;
#endif
            auto begin = stl11::chrono::high_resolution_clock::now();
            while(!failed && stl11::chrono::duration_cast<stl11::chrono::seconds>(stl11::chrono::high_resolution_clock::now() - begin).count() < (20 / BOOST_AFIO_STORAGE_PROFILE_TIME_DIVIDER))
            {
              stl11::this_thread::sleep_for(stl11::chrono::seconds(1));
            }
            done = true;
            for(auto &writer : writers)
              writer.join();
            for(auto &reader : readers)
              reader.join();
            sp.max_aligned_atomic_rewrite.value = max_aligned_atomic_rewrite;
            if(failed)
              return make_valued_outcome<void>();
          }
        }
      }
      catch(...)
      {
        return std::current_exception();
      }
      return make_valued_outcome<void>();
    }

    outcome<void> atomic_rewrite_offset_boundary(storage_profile &sp, file_handle &srch) noexcept
    {
      try
      {
        using off_t = io_service::extent_type;
        size_t size = (size_t) sp.max_aligned_atomic_rewrite.value;
        size_t maxsize = (size_t) sp.max_aligned_atomic_rewrite.value;
        if(size > 1024)
          size = 1024;
        if(maxsize > 8192)
          maxsize = 8192;
        sp.atomic_rewrite_offset_boundary.value = (off_t) -1;
        if(size > 1)
        {
          for(; size <= maxsize; size = size * 2)
          {
            for(off_t offset = 512; offset < size; offset += 512)
            {
              // Create two concurrent writer threads and as many reader threads as additional CPU cores
              std::vector<std::thread> writers, readers;
              std::atomic<size_t> done(2);
              for(char no = '1'; no <= '2'; no++)
                writers.push_back(std::thread([size, offset, &srch, no, &done] {
                  auto _h(srch.clone());
                  if(!_h)
                    throw std::runtime_error("concurrency::atomic_rewrite_offset_boundary: Could not open work file due to " + _h.get_error().message());
                  file_handle h(std::move(_h.get()));
                  std::vector<char> buffer(size, no);
                  file_handle::const_buffer_type _reqs[1] = {std::make_pair(buffer.data(), size)};
                  file_handle::io_request<file_handle::const_buffers_type> reqs(_reqs, offset);
                  --done;
                  while(done)
                    std::this_thread::yield();
                  while(!done)
                  {
                    h.write(reqs);
                  }
                }));
              // Wait till the writers launch
              while(done)
                std::this_thread::yield();
              unsigned concurrency = std::thread::hardware_concurrency() - 2;
              if(concurrency < 4)
                concurrency = 4;
              std::atomic<io_service::extent_type> atomic_rewrite_offset_boundary(sp.atomic_rewrite_offset_boundary.value);
              std::atomic<bool> failed(false);
              for(unsigned no = 0; no < concurrency; no++)
                readers.push_back(std::thread([size, offset, &srch, no, &done, &atomic_rewrite_offset_boundary, &failed] {
                  auto _h(srch.clone());
                  if(!_h)
                    throw std::runtime_error("concurrency::atomic_rewrite_offset_boundary: Could not open work file due to " + _h.get_error().message());
                  file_handle h(std::move(_h.get()));
                  std::vector<char> buffer(size, 0), tocmp(size, 0);
                  file_handle::buffer_type _reqs[1] = {std::make_pair(buffer.data(), size)};
                  file_handle::io_request<file_handle::buffers_type> reqs(_reqs, offset);
                  while(!done)
                  {
                    h.read(reqs);
                    // memset(tocmp.data(), buffer.front(), size);
                    // if (memcmp(buffer.data(), tocmp.data(), size))
                    {
                      const size_t *data = (size_t *) buffer.data(), *end = (size_t *) (buffer.data() + size);
                      for(const size_t *d = data; d < end; d++)
                      {
                        if(*d != *data)
                        {
                          failed = true;
                          off_t failedat = (d - data) + offset;
                          if(failedat < atomic_rewrite_offset_boundary)
                          {
#ifndef NDEBUG
                            std::cout << "  Torn rewrite at offset " << failedat << std::endl;
#endif
                            atomic_rewrite_offset_boundary = failedat;
                          }
                          break;
                        }
                      }
                    }
                  }
                }));

#ifndef NDEBUG
              std::cout << "direct=" << !srch.are_reads_from_cache() << " sync=" << srch.are_writes_durable() << " testing atomicity of rewrites of " << size << " bytes to offset " << offset << " ..." << std::endl;
#endif
              auto begin = stl11::chrono::high_resolution_clock::now();
              while(!failed && stl11::chrono::duration_cast<stl11::chrono::seconds>(stl11::chrono::high_resolution_clock::now() - begin).count() < (20 / BOOST_AFIO_STORAGE_PROFILE_TIME_DIVIDER))
              {
                stl11::this_thread::sleep_for(stl11::chrono::seconds(1));
              }
              done = true;
              for(auto &writer : writers)
                writer.join();
              for(auto &reader : readers)
                reader.join();
              sp.atomic_rewrite_offset_boundary.value = atomic_rewrite_offset_boundary;
              if(failed)
                return make_valued_outcome<void>();
            }
          }
        }
      }
      catch(...)
      {
        return std::current_exception();
      }
      return make_valued_outcome<void>();
    }
  }
#ifdef _MSC_VER
#pragma warning(pop)
#endif
}
BOOST_AFIO_V2_NAMESPACE_END

#ifdef WIN32
#include "windows/storage_profile.ipp"
#else
#include "posix/storage_profile.ipp"
#endif
