/* -*-c++-*- */
/* osgEarth - Dynamic map generation toolkit for OpenSceneGraph
 * Copyright 2008-2010 Pelican Mapping
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
#include <osgEarthFeatures/SubstituteModelFilter>
#include <osgEarthFeatures/MarkerFactory>
#include <osgEarthSymbology/MeshConsolidator>
#include <osgEarth/HTTPClient>
#include <osgEarth/ECEF>
#include <osg/Drawable>
#include <osg/Geode>
#include <osg/MatrixTransform>
#include <osg/NodeVisitor>
#include <osgUtil/Optimizer>
#include <list>
#include <deque>

#define LC "[SubstituteModelFilter] "

using namespace osgEarth;
using namespace osgEarth::Features;
using namespace osgEarth::Symbology;

//------------------------------------------------------------------------

namespace
{
    static osg::Node* s_defaultModel =0L;
}

//------------------------------------------------------------------------

SubstituteModelFilter::SubstituteModelFilter( const Style& style ) :
_style   ( style ),
_cluster ( false ),
_merge   ( true )
{
    //NOP
}

bool
SubstituteModelFilter::process(const FeatureList&           features,                               
                               const MarkerSymbol*          symbol,
                               Session*                     session,
                               osg::Group*                  attachPoint,
                               FilterContext&               context )
{    
    bool makeECEF = context.getSession()->getMapInfo().isGeocentric();

    for( FeatureList::const_iterator f = features.begin(); f != features.end(); ++f )
    {
        Feature* input = f->get();

        GeometryIterator gi( input->getGeometry(), false );
        while( gi.hasMore() )
        {
            Geometry* geom = gi.next();

            for( unsigned i=0; i<geom->size(); ++i )
            {
                osg::Matrixd mat;

                osg::Vec3d point = (*geom)[i];
                if ( makeECEF )
                {
                    // the "rotation" element lets us re-orient the instance to ensure it's pointing up. We
                    // could take a shortcut and just use the current extent's local2world matrix for this,
                    // but it the tile is big enough the up vectors won't be quite right.
                    osg::Matrixd rotation;
                    ECEF::transformAndGetRotationMatrix( context.profile()->getSRS(), point, point, rotation );
                    mat = rotation * _modelMatrix * osg::Matrixd::translate( point ) * _world2local;
                }
                else
                {
                    mat = _modelMatrix * osg::Matrixd::translate( point ) * _world2local;
                }

                osg::MatrixTransform* xform = new osg::MatrixTransform();
                xform->setMatrix( mat );
                xform->setDataVariance( osg::Object::STATIC );
                MarkerFactory factory( session);
                osg::ref_ptr< osg::Node > model = factory.getOrCreateNode( input, symbol );
                if (model.get())
                {
                    xform->addChild( model.get() );
                }

                attachPoint->addChild( xform );

                // name the feature if necessary
                if ( !_featureNameExpr.empty() )
                {
                    const std::string& name = input->eval( _featureNameExpr );
                    if ( !name.empty() )
                        xform->setName( name );
                }
            }
        }
    }

    return true;
}




struct ClusterVisitor : public osg::NodeVisitor
    {
        ClusterVisitor( const FeatureList& features, const osg::Matrixd& modelMatrix, FeaturesToNodeFilter* f2n, FilterContext& cx )
            : _features   ( features ),
              _modelMatrix( modelMatrix ),
              _f2n        ( f2n ),
              _cx         ( cx ),
              osg::NodeVisitor( osg::NodeVisitor::TRAVERSE_ALL_CHILDREN )
        {
            //nop
        }

        void apply( osg::Geode& geode )
        {
            bool makeECEF = _cx.getSession()->getMapInfo().isGeocentric();
            const SpatialReference* srs = _cx.profile()->getSRS();

            // save the geode's drawables..
            osg::Geode::DrawableList old_drawables = geode.getDrawableList();

            // ..and clear out the drawables list.
            geode.removeDrawables( 0, geode.getNumDrawables() );

            // foreach each drawable that was originally in the geode...
            for( osg::Geode::DrawableList::iterator i = old_drawables.begin(); i != old_drawables.end(); i++ )
            {
                osg::Geometry* originalDrawable = dynamic_cast<osg::Geometry*>( i->get() );
                if ( !originalDrawable )
                    continue;

                // go through the list of input features...
                for( FeatureList::const_iterator j = _features.begin(); j != _features.end(); j++ )
                {
                    const Feature* feature = j->get();

                    ConstGeometryIterator gi( feature->getGeometry(), false );
                    while( gi.hasMore() )
                    {
                        const Geometry* geom = gi.next();

                        for( Geometry::const_iterator k = geom->begin(); k != geom->end(); ++k )
                        {
                            osg::Vec3d   point = *k;
                            osg::Matrixd mat;

                            if ( makeECEF )
                            {
                                osg::Matrixd rotation;
                                ECEF::transformAndGetRotationMatrix( srs, point, point, rotation );
                                mat = rotation * _modelMatrix * osg::Matrixd::translate(point) * _f2n->world2local();
                            }
                            else
                            {
                                mat = _modelMatrix * osg::Matrixd::translate(point) * _f2n->world2local();
                            }

                            // clone the source drawable once for each input feature.
                            osg::ref_ptr<osg::Geometry> newDrawable = osg::clone( 
                                originalDrawable, 
                                osg::CopyOp::DEEP_COPY_ARRAYS | osg::CopyOp::DEEP_COPY_PRIMITIVES );

                            osg::Vec3Array* verts = dynamic_cast<osg::Vec3Array*>( newDrawable->getVertexArray() );
                            if ( verts )
                            {
                                for( osg::Vec3Array::iterator v = verts->begin(); v != verts->end(); ++v )
                                {
                                    (*v).set( (*v) * mat );
                                }
                                
                                // add the new cloned, translated drawable back to the geode.
                                geode.addDrawable( newDrawable.get() );
                            }
                        }

                    }
                }
            }

            geode.dirtyBound();

            MeshConsolidator::run( geode );

            // merge the geometry. Not sure this is necessary
            osgUtil::Optimizer opt;
            opt.optimize( &geode, osgUtil::Optimizer::MERGE_GEOMETRY | osgUtil::Optimizer::SHARE_DUPLICATE_STATE );
            
            osg::NodeVisitor::apply( geode );
        }

    private:
        const FeatureList&    _features;
        FilterContext         _cx;
        osg::Matrixd          _modelMatrix;
        FeaturesToNodeFilter* _f2n;
    };


typedef std::map< osg::ref_ptr< osg::Node >, FeatureList > MarkerToFeatures;

//clustering:
//  troll the external model for geodes. for each geode, create a geode in the target
//  model. then, for each geometry in that geode, replicate it once for each instance of
//  the model in the feature batch and transform the actual verts to a local offset
//  relative to the tile centroid. Finally, reassemble all the geodes and optimize. 
//  hopefully stateset sharing etc will work out. we may need to strip out LODs too.
bool
SubstituteModelFilter::cluster(const FeatureList&           features,
                               const MarkerSymbol*          symbol, 
                               Session* session,
                               osg::Group*                  attachPoint,
                               FilterContext&               cx )
{    
    MarkerToFeatures markerToFeatures;
    MarkerFactory factory( session);
    for (FeatureList::const_iterator i = features.begin(); i != features.end(); ++i)
    {
        Feature* f = i->get();
        osg::ref_ptr< osg::Node > model = factory.getOrCreateNode( f, symbol );
        //Try to find the existing entry
        if (model.valid())
        {
            MarkerToFeatures::iterator itr = markerToFeatures.find( model.get() );
            if (itr == markerToFeatures.end())
            {
                markerToFeatures[ model.get() ].push_back( f );
            }
            else
            {
                itr->second.push_back( f );
            }
        }
    }

    /*
    OE_NOTICE << "Sorted models into " << markerToFeatures.size() << " buckets" << std::endl;
    for (MarkerToFeatures::iterator i = markerToFeatures.begin(); i != markerToFeatures.end(); ++i)
    {
        OE_NOTICE << "    Bucket has " << i->second.size() << " features" << std::endl;
    }
    */

    //For each model, cluster the features that use that model

    for (MarkerToFeatures::iterator i = markerToFeatures.begin(); i != markerToFeatures.end(); ++i)
    {
        osg::Node* clone = dynamic_cast<osg::Node*>( i->first->clone( osg::CopyOp::DEEP_COPY_ALL ) );

        // ..and apply the clustering to the copy.
        ClusterVisitor cv( i->second, _modelMatrix, this, cx );
        clone->accept( cv );

        attachPoint->addChild( clone );
    }

    return true;
}

osg::Node*
SubstituteModelFilter::push(FeatureList& features, FilterContext& context)
{
    if ( !isSupported() ) {
        OE_WARN << "SubstituteModelFilter support not enabled" << std::endl;
        return 0L;
    }

    if ( _style.empty() ) {
        OE_WARN << LC << "Empty style; cannot process features" << std::endl;
        return 0L;
    }

    const MarkerSymbol* symbol = _style.getSymbol<MarkerSymbol>();
    if ( !symbol ) {
        OE_WARN << LC << "No MarkerSymbol found in style; cannot process feautres" << std::endl;
        return 0L;
    }

    FilterContext newContext( context );

    computeLocalizers( context );

    osg::Group* group = createDelocalizeGroup(); //new osg::Group();

    bool ok = true;

    if ( _cluster )
    {
        ok = cluster( features, symbol, context.getSession(), group, newContext );
    }

    else
    {
        process( features, symbol, context.getSession(), group, newContext );

#if 0
        // speeds things up a bit, at the expense of creating tons of geometry..

        // this optimizer pass will remove all the MatrixTransform nodes that we
        // used to offset each instance
        osgUtil::Optimizer optimizer;
        optimizer.optimize( group, osgUtil::Optimizer::FLATTEN_STATIC_TRANSFORMS_DUPLICATING_SHARED_SUBGRAPHS );
#endif

    }

    return group;
}
