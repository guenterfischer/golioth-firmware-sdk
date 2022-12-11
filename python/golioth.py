import ctypes
import json

# golioth_tls_auth_type_t enum
GOLIOTH_TLS_AUTH_TYPE_PSK = 0
GOLIOTH_TLS_AUTH_TYPE_PKI = 1

class golioth_psk_credentials_t(ctypes.Structure):
    _fields_ = [
        ("psk_id", ctypes.c_char_p),
        ("psk_id_len", ctypes.c_size_t),
        ("psk", ctypes.c_char_p),
        ("psk_len", ctypes.c_size_t),
    ]

class golioth_pki_credentials_t(ctypes.Structure):
    _fields_ = [
        ("ca_cert", ctypes.c_char_p),
        ("ca_cert_len", ctypes.c_size_t),
        ("public_cert", ctypes.c_char_p),
        ("public_cert_len", ctypes.c_size_t),
        ("private_key", ctypes.c_char_p),
        ("private_key_len", ctypes.c_size_t),
    ]

class golioth_tls_credentials_union(ctypes.Union):
    _fields_ = [
        ("psk", golioth_psk_credentials_t),
        ("pki", golioth_pki_credentials_t),
    ]

class golioth_tls_credentials_t(ctypes.Structure):
    _anonymous_ = ("u",)
    _fields_ = [
        ("auth_type", ctypes.c_int),
        ("u", golioth_tls_credentials_union),
    ]

class golioth_client_config_t(ctypes.Structure):
    _fields_ = [
        ("credentials", golioth_tls_credentials_t),
    ]

class LightDB():
    def __init__(self, client, lib):
        self.timeout = 3000
        self.client = client
        self.lib = lib
        self.__setup_api()

    def __setup_api(self):
        self.lib.golioth_lightdb_set_int_sync.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_int]
        self.lib.golioth_lightdb_set_int_sync.restype = ctypes.c_int

        self.lib.golioth_lightdb_set_bool_sync.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_bool, ctypes.c_int]
        self.lib.golioth_lightdb_set_bool_sync.restype = ctypes.c_int

        self.lib.golioth_lightdb_set_float_sync.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_float, ctypes.c_int]
        self.lib.golioth_lightdb_set_float_sync.restype = ctypes.c_int

        self.lib.golioth_lightdb_set_string_sync.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_int]
        self.lib.golioth_lightdb_set_string_sync.restype = ctypes.c_int

        self.lib.golioth_lightdb_set_json_sync.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_int]
        self.lib.golioth_lightdb_set_json_sync.restype = ctypes.c_int

    def set(self, path, val):
        path = path.encode('utf-8')
        if type(val) is int:
            self.lib.golioth_lightdb_set_int_sync(self.client, path, val, self.timeout)
        if type(val) is bool:
            self.lib.golioth_lightdb_set_bool_sync(self.client, path, val, self.timeout)
        elif type(val) is str:
            val = val.encode('utf-8')
            self.lib.golioth_lightdb_set_string_sync(self.client, path, val, len(val), self.timeout)
        elif type(val) is float:
            self.lib.golioth_lightdb_set_float_sync(self.client, path, val, self.timeout)
        elif type(val) is dict:
            val = json.dumps(val)
            val = val.encode('utf-8')
            self.lib.golioth_lightdb_set_json_sync(self.client, path, val, len(val), self.timeout)

class GoliothClient():
    def __init__(self, psk_id, psk):
        self.lib = ctypes.CDLL("../examples/linux/golioth_basics/build/build/libgolioth_sdk.so")
        self.__setup_api()
        self.psk_id = psk_id.encode('utf-8')
        self.psk = psk.encode('utf-8')
        self.client = self.__create_client()
        self.lib.golioth_client_wait_for_connect(self.client, 5000)
        self.lightdb = LightDB(self.client, self.lib)

    def __create_client(self):
        config = golioth_client_config_t()
        config.credentials = golioth_tls_credentials_t()
        config.credentials.auth_type = GOLIOTH_TLS_AUTH_TYPE_PSK
        config.credentials.psk = golioth_psk_credentials_t()
        config.credentials.psk.psk_id = ctypes.cast(ctypes.create_string_buffer(self.psk_id), ctypes.c_char_p)
        config.credentials.psk.psk_id_len = len(self.psk_id)
        config.credentials.psk.psk = ctypes.cast(ctypes.create_string_buffer(self.psk), ctypes.c_char_p)
        config.credentials.psk.psk_len = len(self.psk)
        return self.lib.golioth_client_create(ctypes.byref(config))

    def __setup_api(self):
        self.lib.golioth_client_create.argtypes = [ctypes.POINTER(golioth_client_config_t)]
        self.lib.golioth_client_create.restype = ctypes.c_void_p

        self.lib.golioth_client_wait_for_connect.argtypes = [ctypes.c_void_p, ctypes.c_int]
        self.lib.golioth_client_wait_for_connect.restype = ctypes.c_bool

# psk_id = '20221028174758-laptop@nicks-first-project'
# psk = 'c3e1a88504de7b53c4e5cce1c28887ad'
# client = GoliothClient(psk_id, psk)
# client.lightdb.set('counter', 0)
# client.lightdb.set('pi', 3.14)
# client.lightdb.set('is_python', True)
# client.lightdb.set('my_psk_id', psk_id)
# data = {
#     'key0': 14,
#     'nested': {
#         'key1': 15
#     },
#     'array': [5,6,7,8]
# }
# client.lightdb.set('structured_data', data)
