# Quick WASAPI capture endpoint channel probe (no JUCE).
# Lists TR-8S-related capture endpoints and max channels via MMDevice/WASAPI activate.

$ErrorActionPreference = "Stop"
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
using System.Collections.Generic;

public static class WasapiProbe
{
    public const int CLSCTX_ALL = 0x17;
    public static readonly Guid CLSID_MMDeviceEnumerator = new Guid("BCDE0395-E52F-467C-8E3D-C4579291692E");
    public static readonly Guid IID_IMMDeviceEnumerator = new Guid("A95664D2-9614-4F35-A746-DE8DB63617E6");
    public static readonly Guid IID_IAudioClient = new Guid("1CB9DF46-3430-4E7B-9097-F0D0E9A3D7F9");
    public static readonly Guid KSDATAFORMAT_SUBTYPE_IEEE_FLOAT = new Guid("00000003-0000-0010-8000-00aa00389b71");

    [ComImport, Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    public interface IMMDeviceEnumerator
    {
        int EnumAudioEndpoints(int dataFlow, int dwStateMask, out IMMDeviceCollection devices);
        int GetDefaultAudioEndpoint(int dataFlow, int role, out IMMDevice endpoint);
        int GetDevice([MarshalAs(UnmanagedType.LPWStr)] string id, out IMMDevice device);
        int RegisterEndpointNotificationCallback(IntPtr client);
        int UnregisterEndpointNotificationCallback(IntPtr client);
    }

    [ComImport, Guid("0BD7A1BE-7A1A-44DB-8393-6C00000C7200"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    public interface IMMDeviceCollection
    {
        int GetCount(out int num);
        int Item(int index, out IMMDevice device);
    }

    [ComImport, Guid("D666063F-1587-4E43-81F1-B948E807363F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    public interface IMMDevice
    {
        int Activate(ref Guid iid, int dwClsCtx, IntPtr pActivationParams, [MarshalAs(UnmanagedType.IUnknown)] out object ppInterface);
        int OpenPropertyStore(int stgmAccess, out IPropertyStore props);
        int GetId([MarshalAs(UnmanagedType.LPWStr)] out string ppstrId);
        int GetState(out int pdwState);
    }

    [ComImport, Guid("886d8eeb-8cf2-4446-8d02-cdba1dbdcf99"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    public interface IPropertyStore
    {
        int GetCount(out int cProps);
        int GetAt(int iProp, out PROPERTYKEY pkey);
        int GetValue(ref PROPERTYKEY key, out PROPVARIANT pv);
        int SetValue(ref PROPERTYKEY key, ref PROPVARIANT pv);
        int Commit();
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct PROPERTYKEY
    {
        public Guid fmtid;
        public int pid;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct PROPVARIANT
    {
        public short vt;
        public short wReserved1, wReserved2, wReserved3;
        public IntPtr p;
        public int p2;
    }

    [ComImport, Guid("1CB9DF46-3430-4E7B-9097-F0D0E9A3D7F9"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    public interface IAudioClient
    {
        int Initialize(int shareMode, int streamFlags, long hnsBufferDuration, long hnsPeriodicity, IntPtr pFormat, ref Guid audioSessionGuid);
        int GetBufferSize(out int bufferSize);
        int GetStreamLatency(out long latency);
        int GetCurrentPadding(out int padding);
        int IsFormatSupported(int shareMode, IntPtr pFormat, out IntPtr closest);
        int GetMixFormat(out IntPtr ppDeviceFormat);
        int GetDevicePeriod(out long def, out long min);
        int Start();
        int Stop();
        int Reset();
        int SetEventHandle(IntPtr eventHandle);
        int GetService(ref Guid riid, [MarshalAs(UnmanagedType.IUnknown)] out object ppv);
    }

    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    public struct WAVEFORMATEX
    {
        public ushort wFormatTag;
        public ushort nChannels;
        public uint nSamplesPerSec;
        public uint nAvgBytesPerSec;
        public ushort nBlockAlign;
        public ushort wBitsPerSample;
        public ushort cbSize;
    }

    public static string PropToString(PROPVARIANT pv)
    {
        // VT_LPWSTR = 31
        if (pv.vt == 31 && pv.p != IntPtr.Zero)
            return Marshal.PtrToStringUni(pv.p);
        return "";
    }

    public static void Run()
    {
        var type = Type.GetTypeFromCLSID(CLSID_MMDeviceEnumerator);
        var enumerator = (IMMDeviceEnumerator)Activator.CreateInstance(type);
        IMMDeviceCollection coll;
        // eCapture = 1, DEVICE_STATE_ACTIVE = 1
        enumerator.EnumAudioEndpoints(1, 1, out coll);
        int count;
        coll.GetCount(out count);
        var nameKey = new PROPERTYKEY { fmtid = new Guid("a45c254e-df1c-4efd-8020-67d146a850e0"), pid = 14 }; // PKEY_Device_FriendlyName

        Console.WriteLine("Active WASAPI capture endpoints:");
        for (int i = 0; i < count; i++)
        {
            IMMDevice dev;
            coll.Item(i, out dev);
            IPropertyStore store;
            dev.OpenPropertyStore(0, out store);
            PROPVARIANT pv;
            store.GetValue(ref nameKey, out pv);
            string name = PropToString(pv);
            if (name.IndexOf("TR-8S", StringComparison.OrdinalIgnoreCase) < 0
                && name.IndexOf("TR8S", StringComparison.OrdinalIgnoreCase) < 0)
                continue;

            object clientObj;
            var iid = IID_IAudioClient;
            int hr = dev.Activate(ref iid, CLSCTX_ALL, IntPtr.Zero, out clientObj);
            int ch = -1; uint rate = 0;
            string note = "";
            if (hr == 0 && clientObj != null)
            {
                var client = (IAudioClient)clientObj;
                IntPtr fmtPtr;
                if (client.GetMixFormat(out fmtPtr) == 0 && fmtPtr != IntPtr.Zero)
                {
                    var fmt = Marshal.PtrToStructure<WAVEFORMATEX>(fmtPtr);
                    ch = fmt.nChannels;
                    rate = fmt.nSamplesPerSec;
                    Marshal.FreeCoTaskMem(fmtPtr);
                }
                else note = "GetMixFormat failed";
                Marshal.ReleaseComObject(clientObj);
            }
            else note = "Activate failed hr=0x" + hr.ToString("X8");

            Console.WriteLine("- " + name + " | mixChannels=" + ch + " | mixRate=" + rate + " | " + note);
            Marshal.ReleaseComObject(store);
            Marshal.ReleaseComObject(dev);
        }
        Marshal.ReleaseComObject(coll);
        Marshal.ReleaseComObject(enumerator);
    }
}
"@

[WasapiProbe]::Run()
