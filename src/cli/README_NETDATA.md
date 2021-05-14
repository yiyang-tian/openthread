# OpenThread CLI - Network Data

## Overview

Thread Network Data contains information about Border Routers and other servers available in the Thread network. Border Routers and devices offering services register their information with the Leader. The Leader collects and structures this information within the Thread Network Data and distributes the information to all devices in the Thread Network.

Border Routers may register prefixes assigned to the Thread Network and prefixes that they offer routes for. Services may register any information relevant to the service itself.

Border Router and service information may be stable or temporary. Stable Thread Network Data is distributed to all devices, including Sleepy End Devices (SEDs). Temporary Network Data is distributed to all nodes except SEDs.

## Quick Start

### Form Network and Configure Prefix

1. Generate and view new network configuration.

   ```bash
   > dataset init new
   Done
   > dataset
   Active Timestamp: 1
   Channel: 13
   Channel Mask: 0x07fff800
   Ext PAN ID: d63e8e3e495ebbc3
   Mesh Local Prefix: fd3d:b50b:f96d:722d::/64
   Master Key: dfd34f0f05cad978ec4e32b0413038ff
   Network Name: OpenThread-8f28
   PAN ID: 0x8f28
   PSKc: c23a76e98f1a6483639b1ac1271e2e27
   Security Policy: 0, onrcb
   Done
   ```

2. Commit new dataset to the Active Operational Dataset in non-volatile storage.

   ```bash
   dataset commit active
   Done
   ```

3. Enable Thread interface

   ```bash
   > ifconfig up
   Done
   > thread start
   Done
   ```

4. Observe IPv6 addresses assigned to the Thread inteface.

   ```bash
   > ipaddr
   fd3d:b50b:f96d:722d:0:ff:fe00:fc00
   fd3d:b50b:f96d:722d:0:ff:fe00:dc00
   fd3d:b50b:f96d:722d:393c:462d:e8d2:db32
   fe80:0:0:0:a40b:197f:593d:ca61
   Done
   ```

5. Register an IPv6 prefix assigned to the Thread network.

   ```bash
   > prefix add fd00:dead:beef:cafe::/64 paros
   Done
   > netdata register
   Done
   ```

6. Observe Thread Network Data.

   ```bash
   > netdata show
   Prefixes:
   fd00:dead:beef:cafe::/64 paros med dc00
   Routes:
   Services:
   Done
   ```

7. Observe IPv6 addresses assigned to the Thread interface.

   ```bash
   > ipaddr
   fd00:dead:beef:cafe:4da8:5234:4aa2:4cfa
   fd3d:b50b:f96d:722d:0:ff:fe00:fc00
   fd3d:b50b:f96d:722d:0:ff:fe00:dc00
   fd3d:b50b:f96d:722d:393c:462d:e8d2:db32
   fe80:0:0:0:a40b:197f:593d:ca61
   Done
   ```

### Attach to Existing Network

Only the Master Key is required for a device to attach to a Thread network.

While not required, specifying the channel avoids the need to search across multiple channels, improving both latency and efficiency of the attach process.

After the device successfully attaches to a Thread network, the device will retrieve the complete Active Operational Dataset.

1. Create a partial Active Operational Dataset.

   ```bash
   > dataset masterkey dfd34f0f05cad978ec4e32b0413038ff
   Done
   > dataset commit active
   Done
   ```

2. Enable Thread interface.

   ```bash
   > ifconfig up
   Done
   > thread start
   Done
   ```

3. After attaching, observe Thread Network Data.

   ```bash
   > netdata show
   Prefixes:
   fd00:dead:beef:cafe::/64 paros med dc00
   Routes:
   Services:
   Done
   ```

4. Observe IPv6 addresses assigned to the Thread interface.

   ```bash
   > ipaddr
   fd00:dead:beef:cafe:4da8:5234:4aa2:4cfa
   fd3d:b50b:f96d:722d:0:ff:fe00:fc00
   fd3d:b50b:f96d:722d:0:ff:fe00:dc00
   fd3d:b50b:f96d:722d:393c:462d:e8d2:db32
   fe80:0:0:0:a40b:197f:593d:ca61
   Done
   ```

## Command List

- [help](#help)
- [register](#register)
- [show](#show)
- [steeringdata](#steeringdata-check-eui64discerner)

## Command Details

### help

Usage: `netdata help`

Print netdata help menu.

```bash
> netdata help
help
register
show
steeringdata
Done
```

### register

Usage: `netdata register`

Register configured prefixes, routes, and services with the Leader.

```bash
> netdata register
Done
```

### show

Usage: `netdata show [-x]`

Print Network Data received from the Leader.

```bash
> netdata show
Prefixes:
fd00:dead:beef:cafe::/64 paros med dc00
Routes:
Services:
Done
```

Print Network Data as hex-encoded TLVs.

```bash
> netdata show -x
08040b02174703140040fd00deadbeefcafe0504dc00330007021140
Done
```

### netdata steeringdata check \<eui64\>|\<discerner\>

Check whether the steering data includes a joiner.

- eui64: The IEEE EUI-64 of the Joiner.
- discerner: The Joiner discerner in format `number/length`.

```bash
> netdata steeringdata check d45e64fa83f81cf7
Done
> netdata steeringdata check 0xabc/12
Done
> netdata steeringdata check 0xdef/12
Error 23: NotFound
```