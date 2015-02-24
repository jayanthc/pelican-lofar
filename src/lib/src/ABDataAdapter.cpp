#include "ABDataAdapter.h"
#include "SpectrumDataSet.h"

using namespace pelican;
using namespace pelican::ampp;

// Construct the signal data adapter.
ABDataAdapter::ABDataAdapter(const ConfigNode& config)
    : AbstractStreamAdapter(config)
{
    // Read the configuration using configuration node utility methods.
    _pktsPerSpec = config.getOption("spectrum", "packets").toUInt();
    _channelsPerPacket = config.getOption("packet", "channels").toUInt();
    _samplesPerPacket = config.getOption("packet", "samples").toUInt();
    _tSamp = config.getOption("samplingTime", "seconds").toFloat();

    // Set up the packet data.
    _packetSize = _headerSize + _channelsPerPacket * 8 + _footerSize;

   // Calculate the total number of channels.
   _nChannels = _pktsPerSpec * _channelsPerPacket;

   // Set the first flag.
   _first = 1;

   // Set missing packet stats.
   _numMissInst = 0;
   _numMissPkts = 0;
}

// Called to de-serialise a chunk of data from the input device.
void ABDataAdapter::deserialise(QIODevice* device)
{
    // A pointer to the data blob to fill should be obtained by calling the
    // dataBlob() inherited method. This returns a pointer to an
    // abstract DataBlob, which should be cast to the appropriate type.
    SpectrumDataSetStokes* blob = (SpectrumDataSetStokes*) dataBlob();

    // Set the size of the data blob to fill.
    // The chunk size is obtained by calling the chunkSize() inherited method.
    unsigned packets = chunkSize() / _packetSize;
    // Number of time samples; Each channel contains 4 pseudo-Stokes values,
    // each of size sizeof(short int)
    unsigned nBlocks = (packets / _pktsPerSpec) * _samplesPerPacket;
    _nPolarisations = 4;
    blob->resize(nBlocks, 1, _nPolarisations, _nChannels);

    // Create a temporary buffer to read out the packet headers, and
    // get the pointer to the data array in the data blob being filled.
    char headerData[_headerSize];
    char d[_pktsPerSpec * (_packetSize - _headerSize - _footerSize)];
    char footerData[_footerSize];

    // Loop over the UDP packets in the chunk.
    float *data = NULL;
    unsigned bytesRead = 0;
    unsigned block = 0;
    signed int specQuart = 0;
    static signed int prevSpecQuart = 0;
    unsigned long int integCount = 0;
    static unsigned long int prevIntegCount = 0;
    unsigned int icDiff = 0;
    unsigned int sqDiff = 0;
    for (unsigned p = 0; p < packets; p++)
    {
        // Ensure there is enough data to read from the device.
        while (device->bytesAvailable() < _packetSize)
        {
            device->waitForReadyRead(-1);
        }
        // Read the packet header from the input device.
        device->read(headerData, _headerSize);

        // Get the packet integration count
        unsigned long int counter = (*((unsigned long int *) headerData)) & 0x0000FFFFFFFFFFFF;
        integCount = (unsigned long int)        // Casting require.
                      (((counter & 0x0000FF0000000000) >> 40)
                     + ((counter & 0x000000FF00000000) >> 24)
                     + ((counter & 0x00000000FF000000) >> 8)
                     + ((counter & 0x0000000000FF0000) << 8)
                     + ((counter & 0x000000000000FF00) << 24)
                     + ((counter & 0x00000000000000FF) << 40));

        // Get the spectral quarter number
        specQuart = (unsigned char) headerData[6];
        if (_first)
        {
            if (specQuart != 0)
            {
                // Ignore the first <= 3 packets
                prevSpecQuart = specQuart;
                prevIntegCount = integCount;
                std::cout << "Spectral quarter = " << specQuart << ". Skipping..." << std::endl;
                // Read the packet data from the input device and discard it.
                bytesRead += device->read(d + bytesRead,
                                          _packetSize - _headerSize - _footerSize);
                // Read the packet footer from the input device and discard it.
                device->read(footerData, _footerSize);
                continue;
            }
            else
            {
                // Reset first flag.
                _first = 0;
                // Reset bytesRead.
                bytesRead = 0;
                // Set the start time of the observation.
                _tStart = 57074.0;      // temp
            }
        }
        else
        {
            if (0 == _integCountStart)
            {
                _integCountStart = integCount;
            }
        }

        // Check for missed packets
        if (((prevSpecQuart + 1) % 4) != specQuart)
        {
            icDiff = integCount - prevIntegCount;
            if (0 == icDiff)    // same integration, different spectral quarter
            {
                sqDiff = specQuart - prevSpecQuart;
                _numMissInst++;
                _numMissPkts = (sqDiff - 1);
                std::cerr << _numMissPkts << " packets dropped!" << std::endl;
            }
            else                // different integration
            {
                _numMissInst++;
                _numMissPkts = ((_pktsPerSpec - 1 - prevSpecQuart)
                                + _pktsPerSpec * (icDiff - 1)
                                + specQuart);
                std::cerr << _numMissPkts << " packets dropped!" << std::endl;
            }
        }
        if (0 == specQuart)
        {
            icDiff = integCount - prevIntegCount;
            if (icDiff != 1)
            {
                _numMissInst++;
                _numMissPkts = ((_pktsPerSpec - 1 - prevSpecQuart)
                                + _pktsPerSpec * (icDiff - 1)
                                + specQuart);
                std::cerr << _numMissPkts << " packets dropped!" << std::endl;
            }
        }
        prevSpecQuart = specQuart;
        prevIntegCount = integCount;

        bytesRead += device->read(d + bytesRead,
                                  _packetSize - _headerSize - _footerSize);
        // Write out spectrum to blob if this is the last spectral quarter.
        signed short int* dd = (signed short int*) d;
        if (_pktsPerSpec - 1 == specQuart)
        {
#if 0
            for (unsigned pol = 0; pol < _nPolarisations; pol++)
            {
                data = (float*) blob->spectrumData(block, 0, pol);
                for (unsigned chan = 0; chan < _nChannels; chan++)
                {
                    data[chan] = (float) dd[chan * 4 + pol];
                }
            }
#else
            // Compute Stokes I and ignore the rest.
            data = (float*) blob->spectrumData(block, 0, 0);
            for (unsigned chan = 0; chan < _nChannels; chan++)
            {
                data[chan] = (float) (dd[chan * 4 + 0]          // XX*
                                      + dd[chan * 4 + 1]);      // YY*
            }
#endif
            memset(d, '\0', _pktsPerSpec * (_packetSize - _headerSize - _footerSize));
            bytesRead = 0;
            block++;
            Q_ASSERT(block <= nBlocks);
        }
        // Read the packet footer from the input device and discard it.
        device->read(footerData, _footerSize);
    }

    std::cout << packets << " packets processed." << std::endl;
    // Set timing
    float timeProcedThisBlock = 0.0;
    if (_pktsPerSpec - 1 == specQuart)
    {
        timeProcedThisBlock = (integCount - _integCountStart) * _pktsPerSpec * _tSamp;
    }
    else
    {
        timeProcedThisBlock = ((integCount - 1 - _integCountStart) * _pktsPerSpec * _tSamp)
                              + (specQuart * _tSamp);
    }
    blob->setLofarTimestamp((_tStart * 86400.0) + timeProcedThisBlock);
    blob->setBlockRate(_tSamp);
}

