/* -*-c++-*- */
/* osgEarth - Dynamic map generation toolkit for OpenSceneGraph
 * Copyright 2008-2009 Pelican Ventures, Inc.
 * http://osgearth.org
 *
 * osgEarth is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 */

#include <osgEarth/TileBuilder>
#include <osgEarth/GeocentricTileBuilder>
#include <osgEarth/ProjectedTileBuilder>
#include <osg/Image>
#include <osg/Notify>
#include <osg/PagedLOD>
#include <osg/ClusterCullingCallback>
#include <osg/CoordinateSystemNode>
#include <osg/TexEnvCombine>
#include <osgFX/MultiTextureControl>
#include <osgDB/ReadFile>
#include <osgTerrain/Terrain>
#include <osgTerrain/TerrainTile>
#include <osgTerrain/Locator>
#include <osgTerrain/GeometryTechnique>
#include <sstream>
#include <stdlib.h>

using namespace osgEarth;

//Store a static list of all the TileBuilder's that have been created.
typedef std::map<std::string, osg::ref_ptr<TileBuilder> > TileBuilderMap;
static TileBuilderMap s_tile_builders;

TileBuilder*
TileBuilder::create( MapConfig* map, const std::string& url_template, const osgDB::ReaderWriter::Options* options )
{
    TileBuilder* result = NULL;
    if ( map )
    {
        osg::ref_ptr<osgDB::ReaderWriter::Options> local_options = options ? 
            new osgDB::ReaderWriter::Options( *local_options ) :
            NULL;

        // transcribe proxy settings:
        if ( !map->getProxyHost().empty() )
        {
            if ( !local_options.valid() )
                local_options = new osgDB::ReaderWriter::Options();

            std::stringstream buf;
            buf << local_options->getOptionString()
                << "OSG_CURL_PROXY=" << map->getProxyHost() << " "
                << "OSG_CURL_PROXYPORT=" << map->getProxyPort();
            local_options->setOptionString( buf.str() );
        }

        osg::notify(osg::INFO) 
            << "[osgEarth] TileBuilder: options string = " 
            << (local_options.valid()? local_options->getOptionString() : "<empty>")
            << std::endl;

        if ( map->getCoordinateSystemType() == MapConfig::CSTYPE_GEOCENTRIC )
        {
            result = new GeocentricTileBuilder( map, url_template, local_options.get() );
        }
        else
        {
            result = new ProjectedTileBuilder( map, url_template, local_options.get() );
        }
    }

    //Cache the tile builder
    s_tile_builders[url_template] = result;

    return result;
}

TileBuilder* TileBuilder::getTileBuilderByUrlTemplate( const std::string &url_template )
{
    TileBuilderMap::const_iterator k = s_tile_builders.find( url_template );
    if (k != s_tile_builders.end()) return k->second.get();
    return 0;
}

osg::Node* TileBuilder::readNode( MapConfig* map)
{
    //Create a fake filename for this MapConfig by converting the pointer to a string and appending ".earth" to it.
    std::stringstream filename;
    filename << &map << ".earth";
    map->setFilename(filename.str());

    //osg::notify(osg::NOTICE) << "MapFilename is " << map->getFilename() << std::endl;

    //Create the TileBuilder
    TileBuilder* tileBuilder = TileBuilder::create(map, map->getFilename());
    if (!tileBuilder->isValid())
        return 0;

    osg::ref_ptr<TileKey> key = tileBuilder->getDataProfile().createTileKey( "" );
    return tileBuilder->createNode(key.get());
}

const TileGridProfile& TileBuilder::getDataProfile()
{
    if (!_profileComputed)
    {        
        for (TileSourceList::iterator itr = image_sources.begin();
            itr != image_sources.end();
            )
        {
            if (_dataProfile.profileType() == TileGridProfile::UNKNOWN)
            {
                //If we don't have a valid profile yet, just set it to the first TileSource in the list
                _dataProfile = (*itr)->getProfile();
            }
            else
            {
                if (_dataProfile != (*itr)->getProfile())
                {
                    //If the current profile is geodetic and the TileSource profile is Mercator, then this is a special case
                    //and we can still use the TileSource.
                    if (!(_dataProfile.profileType() == TileGridProfile::GLOBAL_GEODETIC &&
                        (*itr)->getProfile().profileType() == TileGridProfile::GLOBAL_MERCATOR))
                    {
                        osg::notify(osg::NOTICE) << "Removing incompatible TileSource " << itr->get()->getName() << std::endl;
                        image_sources.erase(itr);
                        continue;
                    }                    
                }
            }
             ++itr;
        }

        for (TileSourceList::iterator itr = heightfield_sources.begin();
            itr != heightfield_sources.end();
            )
        {
            if (_dataProfile.profileType() == TileGridProfile::UNKNOWN)
            {
                //If we don't have a valid profile yet, just set it to the first TileSource in the list
                _dataProfile = (*itr)->getProfile();
            }
            else
            {
                if (_dataProfile != (*itr)->getProfile())
                {
                    osg::notify(osg::NOTICE) << "Removing incompatible TileSource " << itr->get()->getName() << std::endl;
                    heightfield_sources.erase(itr);
                    continue;
                }
            }
             ++itr;
        }

        _profileComputed = true;
    }
    return _dataProfile;
}

static void
addSources(const MapConfig* mapConfig, const SourceConfigList& from, 
           std::vector< osg::ref_ptr<TileSource> >& to,
           const std::string& url_template,
           const osgDB::ReaderWriter::Options* global_options)
{        

    for( SourceConfigList::const_iterator i = from.begin(); i != from.end(); i++ )
    {
        SourceConfig* source = i->get();

        osg::ref_ptr<osgDB::ReaderWriter::Options> local_options = global_options ?
            new osgDB::ReaderWriter::Options( *global_options ) : 
            new osgDB::ReaderWriter::Options();

        //Setup the plugin options for the source
        for( SourceProperties::const_iterator p = source->getProperties().begin(); p != source->getProperties().end(); p++ )
        {
            local_options->setPluginData( p->first, (void*)p->second.c_str() );
        }

        //Give plugins access to the MapConfig object
        local_options->setPluginData("map_config", (void*)mapConfig); 

        bool foundValidSource = false;
        osg::ref_ptr<TileSource> tile_source;
        //Only load the source if we are not running offline
        if (!mapConfig->getCacheOnly())
        {
            //Add the source to the list.  The "." prefix causes OSG to select the correct plugin.
            //For instance, the WMS plugin can be loaded by using ".osgearth_wms" as the filename
            tile_source = dynamic_cast<TileSource*>(osgDB::readObjectFile(".osgearth_" + source->getDriver(), local_options.get()));
            if (!tile_source.valid())
            {
              osg::notify(osg::NOTICE) << "Warning:  Could not load TileSource from "  << source->getDriver() << std::endl;
            }
        }

        if (tile_source.valid())
        {           
            //Initialize the source and set its name
            tile_source->init(local_options.get());
            tile_source->setName( source->getName() );
            osg::notify(osg::INFO) << "Loaded " << source->getDriver() << " TileSource" << std::endl;
        }

        //Configure the cache if necessary
        osg::ref_ptr<const CacheConfig> cacheConfig = source->getCacheConfig() ? source->getCacheConfig() : mapConfig->getCacheConfig();

        osg::ref_ptr<TileSource> sourceToAdd = tile_source;

        //If the cache config is valid, wrap the TileSource with a caching TileSource.
        if (cacheConfig.valid())
        {
            osg::ref_ptr<CachedTileSource> cache = CachedTileSourceFactory::create(tile_source.get(), cacheConfig->getType(), cacheConfig->getProperties());
            if (cache.valid())
            {
                cache->init(local_options.get());
                cache->setName(source->getName());
                cache->setMapConfigFilename( mapConfig->getFilename() );
                cache->initTileMap();
                sourceToAdd = cache.get();
            }
        }

        if (sourceToAdd.valid() && sourceToAdd->getProfile().profileType() != TileGridProfile::UNKNOWN)
        {
            to.push_back( sourceToAdd.get() );
        }
        else
        {
            osg::notify(osg::NOTICE) << "Skipping TileSource with unknown profile " << source->getName() << std::endl;
        }
    }
}

TileBuilder::TileBuilder(MapConfig* _map, 
                         const std::string& _url_template,
                         const osgDB::ReaderWriter::Options* options ) :
map( _map ),
url_template( _url_template ),
_profileComputed(false),
_dataProfile(TileGridProfile::UNKNOWN)
{
    if ( map.valid() )
    {
        addSources( map.get(), map->getImageSources(), image_sources, url_template, options );
        addSources( map.get(), map->getHeightFieldSources(), heightfield_sources, url_template, options );
    }

    if ( map->getProfile() == STR_GLOBAL_MERCATOR )
    {
        osg::notify(osg::NOTICE) << "Overriding profile to GLOBAL_MERCATOR due to profile in MapConfig" << std::endl;
        _dataProfile = TileGridProfile(TileGridProfile::GLOBAL_MERCATOR);
    }
    else if ( map->getProfile() == STR_GLOBAL_GEODETIC )
    {
        osg::notify(osg::NOTICE) << "Overriding profile to GLOBAL_GEODETIC due to profile in MapConfig" << std::endl;
        _dataProfile = TileGridProfile(TileGridProfile::GLOBAL_GEODETIC);
    }
}

std::string
TileBuilder::createURI( const TileKey* key )
{
    return key->str() + "." + url_template;
}

MapConfig*
TileBuilder::getMapConfig() const
{
    return map.get();
}


// Make a MatrixTransform suitable for use with a Locator object based on the given extents.
// Calling Locator::setTransformAsExtents doesn't work with OSG 2.6 due to the fact that the
// _inverse member isn't updated properly.  Calling Locator::setTransform works correctly.
osg::Matrixd
TileBuilder::getTransformFromExtents(double minX, double minY, double maxX, double maxY) const
{
    osg::Matrixd transform;
    transform.set(
        maxX-minX, 0.0,       0.0, 0.0,
        0.0,       maxY-minY, 0.0, 0.0,
        0.0,       0.0,       1.0, 0.0,
        minX,      minY,      0.0, 1.0); 
    return transform;
}


bool
TileBuilder::isValid()
{
    if (image_sources.size() == 0 && heightfield_sources.size() == 0)
    {
        osg::notify(osg::NOTICE) << "Error:  TileBuilder does not contain any image or heightfield sources." << std::endl;
        return false;
    }

    //Check to see if we are trying to do a Geocentric database with a Projected profile.
    if (getDataProfile().profileType() == TileGridProfile::PROJECTED &&
        map->getCoordinateSystemType() == MapConfig::CSTYPE_GEOCENTRIC)
    {
        osg::notify(osg::NOTICE) << "Error:  Cannot create a geocentric scene using projected datasources.  Please specify type=\"flat\" on the map element in the .earth file." << std::endl;
        return false;
    }

    if (getDataProfile().profileType() == TileGridProfile::UNKNOWN)
    {
        osg::notify(osg::NOTICE) << "Error:  Unknown profile" << std::endl;
        return false;
    }

    //Other cases?
    return true;
}



osg::Node*
TileBuilder::createNode( const TileKey* key )
{
    osg::ref_ptr<osg::Group> top;
    osg::Group* parent = NULL;

    //osg::notify(osg::NOTICE) << "[osgEarth] TileBuilder::createNode( " << key->str() << ")" << std::endl;

    if ( key->getLevelOfDetail() == 0 )
    {
        // Note: CSN must always be at the top
        osg::CoordinateSystemNode* csn = createCoordinateSystemNode();
        parent = csn;
        top = csn;

        //If there is more than one image source, use TexEnvCombine to blend them together
        if ( map->getImageSources().size() > 1 )
        {
#if 1
            osg::StateSet* stateset = parent->getOrCreateStateSet();
            for (unsigned int i = 0; i < map->getImageSources().size(); ++i)
            {    
                //Blend the textures together from the bottom up
                stateset->setTextureMode(i, GL_TEXTURE_2D, osg::StateAttribute::ON);
                
                //Interpolate the current texture with the previous combiner result using the textures SRC_ALPHA
                osg::TexEnvCombine * tec = new osg::TexEnvCombine;
                tec->setCombine_RGB(osg::TexEnvCombine::INTERPOLATE);
                
                tec->setSource0_RGB(osg::TexEnvCombine::TEXTURE);
                tec->setOperand0_RGB(osg::TexEnvCombine::SRC_COLOR);

                tec->setSource1_RGB(osg::TexEnvCombine::PREVIOUS);
                tec->setOperand1_RGB(osg::TexEnvCombine::SRC_COLOR);
                
                tec->setSource2_RGB(osg::TexEnvCombine::TEXTURE);
                tec->setOperand2_RGB(osg::TexEnvCombine::SRC_ALPHA);

                stateset->setTextureAttribute(i, tec, osg::StateAttribute::ON);
            }

            //Modulate the result with the primary color to get proper lighting
            osg::TexEnvCombine* texenv = new osg::TexEnvCombine;
            texenv->setCombine_RGB(osg::TexEnvCombine::MODULATE);
            texenv->setSource0_RGB(osg::TexEnvCombine::PREVIOUS);
            texenv->setOperand0_RGB(osg::TexEnvCombine::SRC_COLOR);
            texenv->setSource1_RGB(osg::TexEnvCombine::PRIMARY_COLOR);
            texenv->setOperand1_RGB(osg::TexEnvCombine::SRC_COLOR);
            stateset->setTextureAttribute(map->getImageSources().size(), texenv, osg::StateAttribute::ON);
            stateset->setTextureMode(map->getImageSources().size(), GL_TEXTURE_2D, osg::StateAttribute::ON);
#else
            //Decorate the scene with a multi-texture control to control blending between textures
            osgFX::MultiTextureControl *mt = new osgFX::MultiTextureControl;
            parent->addChild( mt );

            float r = 1.0f/ map->getImageSources().size();
            for (unsigned int i = 0; i < map->getImageSources().size(); ++i)
            {
                mt->setTextureWeight(i, r);
            }
            parent = mt;
#endif
        }

        terrain = new osgEarth::EarthTerrain;//new osgTerrain::Terrain();
        terrain->setVerticalScale( map->getVerticalScale() );
        parent->addChild( terrain.get() );
        parent = terrain.get();
    }
    else
    {
        top = new osg::Group();
        top->setName( key->str() );
        parent = top.get();
    }

    if (!addChildren( parent, key ))
    {
        top = 0;
    }

    return top.release();
}

osg::HeightField*
TileBuilder::createValidHeightField(osgEarth::TileSource* tileSource, const osgEarth::TileKey *key)
{
    //Try to create the heightfield with the given key
    osg::ref_ptr<osg::HeightField> hf;
    osg::ref_ptr<const TileKey> hf_key = key;
    hf = tileSource->createHeightField( key );        

    if (!hf.valid())
    {
        //We could not load the heightfield from the given key, so try to load from parent tiles
        hf_key = key->createParentKey();

        while (hf_key.valid())
        {
            hf = tileSource->createHeightField(hf_key.get());
            if (hf.valid()) break;
            hf_key = hf_key->createParentKey();
        }

        //Use a HeightFieldExtractor to sample the parent tile
        if (hf.valid())
        {
            osg::ref_ptr<HeightFieldExtractor> hfe = new HeightFieldExtractor(hf_key.get(), hf.get());
            hf = hfe->extractChild(key, hf->getNumColumns(), hf->getNumRows());
        }
    }

    return hf.release();
}

bool
TileBuilder::createValidImage(osgEarth::TileSource* tileSource, const osgEarth::TileKey *key, osgEarth::TileBuilder::ImageTileKeyPair &imageTile)
{
    //Try to create the image with the given key
    osg::ref_ptr<osg::Image> image = tileSource->createImage(key);
    osg::ref_ptr<const TileKey> image_key = key;
    
    if (!image.valid())
    {
        //Could not directly create the image from the given TileKey, so try to load from parent tiles
        image_key = key->createParentKey();

        while (image_key.valid())
        {
            image = tileSource->createImage(image_key.get());
            if (image.valid()) break;
            image_key = image_key->createParentKey();
        }
    }

    if (image.valid())
    {
        imageTile.first = image.get();
        imageTile.second = image_key.get();
        return true;
    }
    return false;
}

