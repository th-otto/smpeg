/*
    SMPEG - SDL MPEG Player Library
    Copyright (C) 1999  Loki Entertainment Software

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Library General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Library General Public License for more details.

    You should have received a copy of the GNU Library General Public
    License along with this library; if not, write to the Free
    Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

/* The generic MPEG stream class */

#include "MPEG.h"
#include "MPEGstream.h"
#include "video/video.h"

/* This is the limit of the quantity of pre-read data */
#define MAX_QUEUE (256 * 1024)

MPEGstream::MPEGstream(MPEGsystem * System, Uint8 Streamid)
{
  system = System;
  streamid = Streamid;
  br = new MPEGlist();
  
  data = 0;
  stop = 0;
  
  preread_size = 0;
  looping = false;
  enabled = true;
  mutex = SDL_CreateMutex();
}

MPEGstream::~MPEGstream()
{
  MPEGlist * newbr;

  SDL_DestroyMutex(mutex);

  /* Free the list */
  for(newbr = br; newbr->Prev(); newbr = newbr->Prev());

  while(newbr->Next())
  {
    newbr = newbr->Next();
    delete newbr->Prev();
  }
  delete newbr;
}

void
MPEGstream::reset_stream()
{
  MPEGlist * newbr;

  SDL_mutexP(mutex);
  /* Seek the first buffer */
  for(newbr = br; newbr->Prev(); newbr = newbr->Prev());
  
  /* Free buffers  */
  while(newbr->Next())
  {
    newbr = newbr->Next();
    delete newbr->Prev();
  } 
  delete newbr;

  br = new MPEGlist();
  data = 0;
  stop = 0;
  looping = false;
  preread_size = 0;
  SDL_mutexV(mutex);
}

void
MPEGstream::rewind_stream()
{
  /* Note that this will rewind all streams, and other streams than this one */
  /* will finish reading their prebuffured data (they are not reseted) */
  /* This should works because there are always sequence start codes or */
  /* audio start codes at the beginning of the streams */
  /* Of course, this won't work on network streams */

  /* Restart the system */
  system->Rewind();
}

bool
MPEGstream:: next_packet(bool recurse)
{
  SDL_mutexP(mutex);

  /* Unlock current buffer */
  br->Unlock();

  /* No more buffer ? */
  if(!br->Next())
  {
    int timeout = 3000;

    /* Then ask the system to read a new buffer */
    SDL_mutexV(mutex);
    system->RequestBuffer();
    while(!br->Next() && timeout--)
      SDL_Delay(1);
    if(!timeout) return(false);
    SDL_mutexP(mutex);
  }

  br = br->Next();
  preread_size -= br->Size();

  /* Check for the end of stream mark */
  if(eof())
  {
    if(looping)
    {
      /* No more buffer ? */
      while(!br->Next())
      {
	/* Then ask the system to read a new buffer */
	SDL_mutexV(mutex);
	system->RequestBuffer();
	SDL_mutexP(mutex);
      }

      /* Skip the eof mark */
      br = br->Next();
      preread_size -= br->Size();
    }
    else
    {
      /* Report eof */
      SDL_mutexV(mutex);
      return(false);
    }
  }

  /* Lock the buffer */
  br->Lock();

  /* Make sure that we have read buffers in advance if possible */
  if(preread_size < MAX_QUEUE)
    system->RequestBuffer();
  
  /* Update stream datas */
  data = (Uint8 *) br->Buffer();
  stop = data + br->Size();

  SDL_mutexV(mutex);

  return(true);
}

MPEGstream_marker *
MPEGstream:: new_marker(int offset)
{
    MPEGstream_marker * marker;

    SDL_mutexP(mutex);
    /* We can't mark past the end of the stream */
    if ( eof() ) {
      SDL_mutexV(mutex);
      return(0);
    }

    /* It may be possible to seek in the data stream, but punt for now */
    if ( ((data+offset) < br->Buffer()) || ((data+offset) > stop) ) {
        SDL_mutexV(mutex);
        return(0);
    }

    /* Set up the mark */
    marker = new MPEGstream_marker;
    marker->marked_buffer = br;
    marker->marked_data = data+offset;
    marker->marked_stop = stop;

    /* Lock the new buffer */
    marker->marked_buffer->Lock();

    SDL_mutexV(mutex);

    return(marker);
}

bool
MPEGstream:: seek_marker(MPEGstream_marker const * marker)
{
    SDL_mutexP(mutex);

    if ( marker ) {
        /* Release current buffer */
        if(br->IsLocked())
	{
	  br->Unlock();
       	  marker->marked_buffer->Lock();
	}

        /* Reset the data positions */
	br = marker->marked_buffer;
        data = marker->marked_data;
        stop = marker->marked_stop;
    }

    SDL_mutexV(mutex);

    return(marker != 0);
}

void
MPEGstream:: delete_marker(MPEGstream_marker *marker)
{
    marker->marked_buffer->Unlock();
    delete marker;
}

Uint32
MPEGstream:: copy_data(Uint8 *area, Sint32 size, bool short_read)
{
    Uint32 copied = 0;

    while ( (size > 0) && !eof()) {
        Uint32 len;

        /* Get new data if necessary */
        if ( data == stop ) {
            if ( ! next_packet() ) {
                break;
            }
        }

	SDL_mutexP(mutex);

        /* Copy as much as we need */
        if ( size <= (Sint32)(stop-data) ) {
            len = size;
        } else {
            len = (stop-data);
        }

        memcpy(area, data, len);

        area += len;
        data += len;
        size -= len;
        copied += len;

        /* Allow 32-bit aligned short reads? */
        if ( ((copied%4) == 0) && short_read ) {
            break;
        }

	SDL_mutexV(mutex);

    }

    return(copied);
}

int MPEGstream::copy_byte(void)
{
  /* Get new data if necessary */
  if ( data == stop ) {
    if ( ! next_packet() ) {
      return (-1);
    }
  }

  return(*data++);
}

bool MPEGstream::eof() const
{
  return(!br->Size());
}

void MPEGstream::insert_packet(Uint8 * Data, Uint32 Size)
{
  MPEGlist * newbr;

  /* Discard all packets if not enabled */
  if(!enabled) return;

  SDL_mutexP(mutex);

  preread_size += Size;

  /* Seek the last buffer */
  for(newbr = br; newbr->Next(); newbr = newbr->Next());

  /* Position ourselves at the end of the stream */
  newbr = newbr->Alloc(Size);

  memcpy(newbr->Buffer(), Data, Size);

  SDL_mutexV(mutex);
  garbage_collect();
}

/* - Check for unused buffers and free them - */
void MPEGstream::garbage_collect(void)
{
  MPEGlist * newbr;

  SDL_mutexP(mutex);  

  br->Lock();

  /* First of all seek the first buffer */
  for(newbr = br; newbr->Prev(); newbr = newbr->Prev());

  /* Now free buffers until we find a locked buffer */
  while(newbr->Next() && !newbr->IsLocked())
  {
    newbr = newbr->Next();
    delete newbr->Prev();
  }

  br->Unlock();

  SDL_mutexV(mutex);
}

void MPEGstream::loop(bool toggle)
{
  looping = toggle;
}

bool MPEGstream::is_looping() const
{
  return(looping);
}

void MPEGstream::enable(bool toggle)
{
  enabled = toggle;
}
