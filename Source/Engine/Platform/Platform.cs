// Copyright (c) Wojciech Figat. All rights reserved.

using System;

namespace FlaxEngine
{
    partial class Platform
    {
        /// <summary>
        /// Checks if current execution in on the main thread.
        /// </summary>
        public static bool IsInMainThread => CurrentThreadID == Globals.MainThreadID;
    }

    partial class Network
    {
        /// <summary>
        /// Writes data to the socket.
        /// </summary>
        /// <param name="socket">The socket.</param>
        /// <param name="data">The data to write.</param>
        /// <returns>Returns -1 on error, otherwise bytes written.</returns>
        [Unmanaged]
        public static unsafe int WriteSocket(NetworkSocket socket, byte[] data)
        {
            if (data == null)
                throw new ArgumentNullException(nameof(data));
            fixed (byte* ptr = data)
                return WriteSocket(socket, (nint)ptr, (uint)data.Length, IntPtr.Zero);
        }

        /// <summary>
        /// Writes data to the socket.
        /// </summary>
        /// <param name="socket">The socket.</param>
        /// <param name="data">The data to write.</param>
        /// <param name="endPoint">If protocol is UDP, the destination end point.</param>
        /// <returns>Returns -1 on error, otherwise bytes written.</returns>
        [Unmanaged]
        public static unsafe int WriteSocket(NetworkSocket socket, byte[] data, NetworkEndPoint endPoint)
        {
            if (data == null)
                throw new ArgumentNullException(nameof(data));
            fixed (byte* ptr = data)
                return WriteSocket(socket, (nint)ptr, (uint)data.Length, (IntPtr)(&endPoint));
        }

        /// <summary>
        /// Reads data on the socket.
        /// </summary>
        /// <param name="socket">The socket.</param>
        /// <param name="buffer">The buffer.</param>
        /// <returns>Returns -1 on error, otherwise bytes read.</returns>
        [Unmanaged]
        public static unsafe int ReadSocket(NetworkSocket socket, byte[] buffer)
        {
            if (buffer == null)
                throw new ArgumentNullException(nameof(buffer));
            fixed (byte* ptr = buffer)
                return ReadSocket(socket, (nint)ptr, (uint)buffer.Length, IntPtr.Zero);
        }

        /// <summary>
        /// Reads data on the socket.
        /// </summary>
        /// <param name="socket">The socket.</param>
        /// <param name="buffer">The buffer.</param>
        /// <param name="endPoint">If UDP, the end point from where data is coming. Otherwise nullptr.</param>
        /// <returns>Returns -1 on error, otherwise bytes read.</returns>
        [Unmanaged]
        public static unsafe int ReadSocket(NetworkSocket socket, byte[] buffer, NetworkEndPoint endPoint)
        {
            if (buffer == null)
                throw new ArgumentNullException(nameof(buffer));
            fixed (byte* ptr = buffer)
                return ReadSocket(socket, (nint)ptr, (uint)buffer.Length, (IntPtr)(&endPoint));
        }
    }
}
