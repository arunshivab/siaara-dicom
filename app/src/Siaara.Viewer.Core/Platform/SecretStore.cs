// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using System.Security.Cryptography;
using System.Text;
using Siaara.Viewer.Core.Audit;
using Siaara.Viewer.Core.Logging;
using Siaara.Viewer.Core.Settings;

namespace Siaara.Viewer.Core.Platform;

/// <summary>Keeps the secrets of network nodes (passwords, bearer tokens, key-file passwords) outside settings.json.</summary>
public interface ISecretStore
{
    /// <summary>How the secrets are protected, for the settings page and the audit log.</summary>
    string Protection { get; }

    /// <summary>Reads a secret.</summary>
    /// <param name="id">Its id (lower-case letters, digits, '.', '-').</param>
    /// <returns>The secret, or null when there is none.</returns>
    string? Read(string id);

    /// <summary>Stores or replaces a secret.</summary>
    /// <param name="id">Its id.</param>
    /// <param name="value">The secret.</param>
    void Write(string id, string value);

    /// <summary>Removes a secret (no error when there is none).</summary>
    /// <param name="id">Its id.</param>
    void Delete(string id);
}

/// <summary>
/// The secret store per OS, with our own P/Invoke (no packages). Windows: one file per secret wrapped
/// by CryptProtectData for the current user. macOS: generic-password items in the login keychain
/// (service "in.sthraas.siaara.viewer.secret"); when the keychain cannot be used, the file form
/// with a warning. Linux and others: one AES-256-GCM file per secret under a 0700 folder, the key
/// in a 0600 file next to them. The Linux form is the weaker one: it keeps secrets out of
/// settings.json, backups of that file and other users' reach, but a program running as the same
/// user can read the key file.
/// </summary>
public static class SecretStores
{
    /// <summary>The store of the running OS.</summary>
    /// <param name="folder">The app's secrets folder (created owner-only).</param>
    /// <returns>The store.</returns>
    public static ISecretStore ForCurrentOs(string folder)
    {
        OwnerOnly.CreateDirectory(folder);
        if (OperatingSystem.IsWindows())
        {
            return new WindowsStore(folder);
        }
        return OperatingSystem.IsMacOS() ? new MacStore(folder) : new FileStore(folder);
    }

    /// <summary>Always the encrypted-file form (what Linux uses; tests).</summary>
    /// <param name="folder">Folder.</param>
    /// <returns>The store.</returns>
    public static ISecretStore EncryptedFiles(string folder)
    {
        OwnerOnly.CreateDirectory(folder);
        return new FileStore(folder);
    }

    internal static string CheckId(string id)
    {
        ArgumentException.ThrowIfNullOrEmpty(id);
        if (id.Length > 80 || !id.All(c => char.IsAsciiLetterLower(c) || char.IsAsciiDigit(c) || c is '.' or '-') || id[0] == '.')
        {
            throw new ArgumentException("Secret ids are lower-case letters, digits, '.' and '-'.", nameof(id));
        }
        return id;
    }

    private sealed class FileStore(string folder) : ISecretStore
    {
        private const int NonceBytes = 12, TagBytes = 16;
        private readonly Lock _gate = new();

        public string Protection => "encrypted file, key in an owner-only file";

        private string PathOf(string id) => Path.Combine(folder, CheckId(id) + ".sec");

        private byte[] Key(bool create)
        {
            string path = Path.Combine(folder, "secret.key");
            if (File.Exists(path))
            {
                if (!OperatingSystem.IsWindows() && !OwnerOnly.IsOwnerOnly(File.GetUnixFileMode(path)))
                {
                    AppLog.Warn("secrets", "the secret key file was readable by others; its mode was set to owner-only. Change the passwords stored for network nodes.");
                    File.SetUnixFileMode(path, UnixFileMode.UserRead | UnixFileMode.UserWrite);
                }
                try
                {
                    byte[] k = Convert.FromHexString(File.ReadAllText(path).Trim());
                    if (k.Length == 32)
                    {
                        return k;
                    }
                }
                catch (FormatException)
                {
                    // Falls through to the error below.
                }
                throw new CryptographicException("The secret key file is damaged.");
            }
            if (!create)
            {
                throw new CryptographicException("The secret key file is missing.");
            }
            byte[] key = RandomNumberGenerator.GetBytes(32);
            AtomicFile.WriteAllBytes(path, Encoding.ASCII.GetBytes(Convert.ToHexStringLower(key)));
            return key;
        }

        public string? Read(string id)
        {
            lock (_gate)
            {
                string path = PathOf(id);
                if (!File.Exists(path))
                {
                    return null;
                }
                byte[] blob = File.ReadAllBytes(path);
                if (blob.Length < 1 + NonceBytes + TagBytes || blob[0] != 1)
                {
                    throw new CryptographicException("A stored secret is damaged.");
                }
                byte[] clear = new byte[blob.Length - 1 - NonceBytes - TagBytes];
                using var aes = new AesGcm(Key(create: false), TagBytes);
                aes.Decrypt(blob.AsSpan(1, NonceBytes), blob.AsSpan(1 + NonceBytes + TagBytes), blob.AsSpan(1 + NonceBytes, TagBytes), clear,
                    Encoding.UTF8.GetBytes(id));
                string value = Encoding.UTF8.GetString(clear);
                CryptographicOperations.ZeroMemory(clear);
                return value;
            }
        }

        public void Write(string id, string value)
        {
            ArgumentNullException.ThrowIfNull(value);
            lock (_gate)
            {
                string path = PathOf(id);
                byte[] clear = Encoding.UTF8.GetBytes(value);
                byte[] blob = new byte[1 + NonceBytes + TagBytes + clear.Length];
                blob[0] = 1;
                RandomNumberGenerator.Fill(blob.AsSpan(1, NonceBytes));
                using var aes = new AesGcm(Key(create: true), TagBytes);
                aes.Encrypt(blob.AsSpan(1, NonceBytes), clear, blob.AsSpan(1 + NonceBytes + TagBytes), blob.AsSpan(1 + NonceBytes, TagBytes),
                    Encoding.UTF8.GetBytes(id));
                CryptographicOperations.ZeroMemory(clear);
                AtomicFile.WriteAllBytes(path, blob);
            }
        }

        public void Delete(string id)
        {
            lock (_gate)
            {
                File.Delete(PathOf(id));
            }
        }
    }

    private sealed class WindowsStore(string folder) : ISecretStore
    {
        public string Protection => "Windows data protection (current user)";

        private string PathOf(string id) => Path.Combine(folder, CheckId(id) + ".dpapi");

        private static byte[] Entropy(string id) => Encoding.UTF8.GetBytes("SIAARA Viewer secret v1|" + id);

        public string? Read(string id)
        {
            string path = PathOf(id);
            if (!OperatingSystem.IsWindows() || !File.Exists(path))
            {
                return null;
            }
            byte[] clear = WindowsDataProtection.Unprotect(File.ReadAllBytes(path), Entropy(id));
            string value = Encoding.UTF8.GetString(clear);
            CryptographicOperations.ZeroMemory(clear);
            return value;
        }

        public void Write(string id, string value)
        {
            ArgumentNullException.ThrowIfNull(value);
            if (!OperatingSystem.IsWindows())
            {
                throw new PlatformNotSupportedException();
            }
            byte[] clear = Encoding.UTF8.GetBytes(value);
            AtomicFile.WriteAllBytes(PathOf(id), WindowsDataProtection.Protect(clear, Entropy(id)));
            CryptographicOperations.ZeroMemory(clear);
        }

        public void Delete(string id) => File.Delete(PathOf(id));
    }

    private sealed class MacStore(string folder) : ISecretStore
    {
        private const string Service = "in.sthraas.siaara.viewer.secret";
        private readonly FileStore _fallback = new(folder);
        private readonly string _suffix = "@" + Convert.ToHexStringLower(SHA256.HashData(Encoding.UTF8.GetBytes(Path.GetFullPath(folder))))[..16];
        private bool _useFiles;

        public string Protection => _useFiles ? _fallback.Protection : "macOS login keychain";

        // Several data folders (tests, a second profile) must not share items: the account carries the folder.
        private string Account(string id) => CheckId(id) + _suffix;

        private T Try<T>(Func<T> keychain, Func<T> files)
        {
            if (_useFiles || !OperatingSystem.IsMacOS())
            {
                return files();
            }
            try
            {
                return keychain();
            }
            catch (Exception e) when (e is CryptographicException or DllNotFoundException or EntryPointNotFoundException)
            {
                AppLog.Warn("secrets", "the keychain could not be used for node secrets; encrypted files are used instead (" + e.GetType().Name + ")");
                _useFiles = true;
                return files();
            }
        }

        public string? Read(string id) => Try(() =>
        {
            if (!OperatingSystem.IsMacOS())
            {
                return null;
            }
            byte[]? v = MacKeychain.Find(Account(id), Service);
            return v is null ? _fallback.Read(id) : Encoding.UTF8.GetString(v);
        }, () => _fallback.Read(id));

        public void Write(string id, string value)
        {
            ArgumentNullException.ThrowIfNull(value);
            Try(() =>
            {
                if (OperatingSystem.IsMacOS())
                {
                    MacKeychain.Delete(Account(id), Service);
                    MacKeychain.Add(Account(id), Encoding.UTF8.GetBytes(value), Service);
                    _fallback.Delete(id);
                }
                return true;
            }, () =>
            {
                _fallback.Write(id, value);
                return true;
            });
        }

        public void Delete(string id)
        {
            _fallback.Delete(id);
            Try(() =>
            {
                if (OperatingSystem.IsMacOS())
                {
                    MacKeychain.Delete(Account(id), Service);
                }
                return true;
            }, () => true);
        }
    }
}
