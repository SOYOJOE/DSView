{
    "Channel Mode": 0,
    "CollectMode": 0,
    "Device": "uart-vcd",
    "DeviceMode": 0,
    "Enable RLE Compress": 0,
    "Filter Targets": 0,
    "Horizontal trigger position": 0,
    "Language": 31,
    "Max Height": "1X",
    "Operation Mode": 1,
    "Sample count": "10000000",
    "Sample rate": "1000000",
    "Stop Options": 1,
    "Threshold Level": "1",
    "Title": "DSView v1.3.2",
    "Trigger channel": 0,
    "Trigger hold off": "0",
    "Trigger margin": 8,
    "Trigger slope": 0,
    "Trigger source": 0,
    "Using Clock Negedge": 0,
    "Using External Clock": 0,
    "Version": 3,
    "channel": [
        {"colour":"default","enabled":true,"index":0,"name":"D0","strigger":0,"type":10000,"view_index":0},
        {"colour":"default","enabled":true,"index":1,"name":"D1","strigger":0,"type":10000,"view_index":1},
        {"colour":"default","enabled":true,"index":2,"name":"D2","strigger":0,"type":10000,"view_index":2},
        {"colour":"default","enabled":true,"index":3,"name":"D3","strigger":0,"type":10000,"view_index":3},
        {"colour":"default","enabled":true,"index":4,"name":"D4","strigger":0,"type":10000,"view_index":4},
        {"colour":"default","enabled":true,"index":5,"name":"D5","strigger":0,"type":10000,"view_index":5},
        {"colour":"default","enabled":true,"index":6,"name":"D6","strigger":0,"type":10000,"view_index":6},
        {"colour":"default","enabled":true,"index":7,"name":"D7","strigger":0,"type":10000,"view_index":7},
        {"colour":"default","enabled":true,"index":8,"name":"D8","strigger":0,"type":10000,"view_index":8},
        {"colour":"default","enabled":true,"index":9,"name":"D9","strigger":0,"type":10000,"view_index":9},
        {"colour":"default","enabled":true,"index":10,"name":"D10","strigger":0,"type":10000,"view_index":10},
        {"colour":"default","enabled":true,"index":11,"name":"D11","strigger":0,"type":10000,"view_index":11},
        {"colour":"default","enabled":true,"index":12,"name":"D12","strigger":0,"type":10000,"view_index":12},
        {"colour":"default","enabled":true,"index":13,"name":"D13","strigger":0,"type":10000,"view_index":13},
        {"colour":"default","enabled":true,"index":14,"name":"D14","strigger":0,"type":10000,"view_index":14},
        {"colour":"default","enabled":true,"index":15,"name":"D15","strigger":0,"type":10000,"view_index":15},
        {"colour":"default","enabled":true,"index":16,"name":"D16","strigger":0,"type":10000,"view_index":16},
        {"colour":"default","enabled":true,"index":17,"name":"D17","strigger":0,"type":10000,"view_index":17},
        {"colour":"default","enabled":true,"index":18,"name":"D18","strigger":0,"type":10000,"view_index":18},
        {"colour":"default","enabled":true,"index":19,"name":"D19","strigger":0,"type":10000,"view_index":19},
        {"colour":"default","enabled":true,"index":20,"name":"D20","strigger":0,"type":10000,"view_index":20},
        {"colour":"default","enabled":true,"index":21,"name":"D21","strigger":0,"type":10000,"view_index":21},
        {"colour":"default","enabled":true,"index":22,"name":"D22","strigger":0,"type":10000,"view_index":22},
        {"colour":"default","enabled":true,"index":23,"name":"D23","strigger":0,"type":10000,"view_index":23},
        {"colour":"default","enabled":true,"index":24,"name":"RX0","strigger":0,"type":10000,"view_index":24},
        {"colour":"default","enabled":true,"index":25,"name":"RX1","strigger":0,"type":10000,"view_index":26},
        {"colour":"default","enabled":true,"index":26,"name":"RX2","strigger":0,"type":10000,"view_index":28},
        {"colour":"default","enabled":true,"index":27,"name":"RX3","strigger":0,"type":10000,"view_index":30},
        {"colour":"default","enabled":true,"index":28,"name":"RX4","strigger":0,"type":10000,"view_index":32},
        {"colour":"default","enabled":true,"index":29,"name":"RX5","strigger":0,"type":10000,"view_index":34},
        {"colour":"default","enabled":true,"index":30,"name":"RX6","strigger":0,"type":10000,"view_index":36},
        {"colour":"default","enabled":true,"index":31,"name":"RX7","strigger":0,"type":10000,"view_index":38}
    ],
    "decoder": [
        {
            "channel": [{"rxtx": 24}],
            "id": "0:uart",
            "label": "0:UART-RX0",
            "options": {
                "anno_startstop": "yes",
                "baudrate": 500000,
                "bit_order": "lsb-first",
                "format": "hex",
                "invert": "no",
                "num_data_bits": 8,
                "num_stop_bits": 1,
                "parity_check": "yes",
                "parity_type": "none"
            },
            "show": {
                "0:uart": true,
                "0:uart: bits": true,
                "0:uart: break": true,
                "0:uart: RX/TX": true,
                "0:uart: RX/TX dump": false,
                "0:uart: warnings": true
            },
            "stacked decoders": [],
            "version": 2,
            "view_index": 25
        },
        {
            "channel": [{"rxtx": 25}],
            "id": "0:uart",
            "label": "1:UART-RX1",
            "options": {
                "anno_startstop": "yes",
                "baudrate": 500000,
                "bit_order": "lsb-first",
                "format": "hex",
                "invert": "no",
                "num_data_bits": 8,
                "num_stop_bits": 1,
                "parity_check": "yes",
                "parity_type": "none"
            },
            "show": {
                "0:uart": true,
                "0:uart: bits": true,
                "0:uart: break": true,
                "0:uart: RX/TX": true,
                "0:uart: RX/TX dump": false,
                "0:uart: warnings": true
            },
            "stacked decoders": [],
            "version": 2,
            "view_index": 27
        },
        {
            "channel": [{"rxtx": 26}],
            "id": "0:uart",
            "label": "2:UART-RX2",
            "options": {
                "anno_startstop": "yes",
                "baudrate": 500000,
                "bit_order": "lsb-first",
                "format": "hex",
                "invert": "no",
                "num_data_bits": 8,
                "num_stop_bits": 1,
                "parity_check": "yes",
                "parity_type": "none"
            },
            "show": {
                "0:uart": true,
                "0:uart: bits": true,
                "0:uart: break": true,
                "0:uart: RX/TX": true,
                "0:uart: RX/TX dump": false,
                "0:uart: warnings": true
            },
            "stacked decoders": [],
            "version": 2,
            "view_index": 29
        },
        {
            "channel": [{"rxtx": 27}],
            "id": "0:uart",
            "label": "3:UART-RX3",
            "options": {
                "anno_startstop": "yes",
                "baudrate": 500000,
                "bit_order": "lsb-first",
                "format": "hex",
                "invert": "no",
                "num_data_bits": 8,
                "num_stop_bits": 1,
                "parity_check": "yes",
                "parity_type": "none"
            },
            "show": {
                "0:uart": true,
                "0:uart: bits": true,
                "0:uart: break": true,
                "0:uart: RX/TX": true,
                "0:uart: RX/TX dump": false,
                "0:uart: warnings": true
            },
            "stacked decoders": [],
            "version": 2,
            "view_index": 31
        },
        {
            "channel": [{"rxtx": 28}],
            "id": "0:uart",
            "label": "4:UART-RX4",
            "options": {
                "anno_startstop": "yes",
                "baudrate": 500000,
                "bit_order": "lsb-first",
                "format": "hex",
                "invert": "no",
                "num_data_bits": 8,
                "num_stop_bits": 1,
                "parity_check": "yes",
                "parity_type": "none"
            },
            "show": {
                "0:uart": true,
                "0:uart: bits": true,
                "0:uart: break": true,
                "0:uart: RX/TX": true,
                "0:uart: RX/TX dump": false,
                "0:uart: warnings": true
            },
            "stacked decoders": [],
            "version": 2,
            "view_index": 33
        },
        {
            "channel": [{"rxtx": 29}],
            "id": "0:uart",
            "label": "5:UART-RX5",
            "options": {
                "anno_startstop": "yes",
                "baudrate": 500000,
                "bit_order": "lsb-first",
                "format": "hex",
                "invert": "no",
                "num_data_bits": 8,
                "num_stop_bits": 1,
                "parity_check": "yes",
                "parity_type": "none"
            },
            "show": {
                "0:uart": true,
                "0:uart: bits": true,
                "0:uart: break": true,
                "0:uart: RX/TX": true,
                "0:uart: RX/TX dump": false,
                "0:uart: warnings": true
            },
            "stacked decoders": [],
            "version": 2,
            "view_index": 35
        },
        {
            "channel": [{"rxtx": 30}],
            "id": "0:uart",
            "label": "6:UART-RX6",
            "options": {
                "anno_startstop": "yes",
                "baudrate": 500000,
                "bit_order": "lsb-first",
                "format": "hex",
                "invert": "no",
                "num_data_bits": 8,
                "num_stop_bits": 1,
                "parity_check": "yes",
                "parity_type": "none"
            },
            "show": {
                "0:uart": true,
                "0:uart: bits": true,
                "0:uart: break": true,
                "0:uart: RX/TX": true,
                "0:uart: RX/TX dump": false,
                "0:uart: warnings": true
            },
            "stacked decoders": [],
            "version": 2,
            "view_index": 37
        },
        {
            "channel": [{"rxtx": 31}],
            "id": "0:uart",
            "label": "7:UART-RX7",
            "options": {
                "anno_startstop": "yes",
                "baudrate": 500000,
                "bit_order": "lsb-first",
                "format": "hex",
                "invert": "no",
                "num_data_bits": 8,
                "num_stop_bits": 1,
                "parity_check": "yes",
                "parity_type": "none"
            },
            "show": {
                "0:uart": true,
                "0:uart: bits": true,
                "0:uart: break": true,
                "0:uart: RX/TX": true,
                "0:uart: RX/TX dump": false,
                "0:uart: warnings": true
            },
            "stacked decoders": [],
            "version": 2,
            "view_index": 39
        }
    ],
    "trigger": {
        "advTriggerMode": false,
        "serialTriggerBits": 0,
        "serialTriggerChannel": 0,
        "serialTriggerClock": "X X X X X X X X X X X X X X X X",
        "serialTriggerData": "X X X X X X X X X X X X X X X X",
        "serialTriggerStart": "X X X X X X X X X X X X X X X X",
        "serialTriggerStop": "X X X X X X X X X X X X X X X X",
        "triggerPos": 1,
        "triggerStages": 0,
        "triggerTab": 0
    }
}
