# Python Reference Client

`boltstream_client.py` is a zero-dependency interoperability example for Python 3.9+.
It implements the protocol-v4-compatible subset accepted by the protocol-v5 broker:
health, authentication, topic creation, single-record produce, and fetch.

```powershell
$env:BOLTSTREAM_BROKER_TOKEN = "local-demo-token"
python clients/python/boltstream_client.py demo --topic python-demo
```

The client deliberately does not implement compressed batches, coordinated consumer
groups, administration, or retry policy. Production applications should build those
policies around the documented protocol or use the C++ client library.

Run the unit suite with:

```powershell
python -m unittest discover -s clients/python/tests -v
```
