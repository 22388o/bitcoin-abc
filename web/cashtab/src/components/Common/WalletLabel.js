import * as React from 'react';
import PropTypes from 'prop-types';
import styled from 'styled-components';

const WalletName = styled.h4`
    font-size: 16px;
    display: inline-block;
    color: ${props => props.theme.lightWhite};
    margin-bottom: 0px;
    @media (max-width: 400px) {
        font-size: 16px;
    }
`;

const WalletLabel = ({ name }) => {
    return (
        <>
            {name && typeof name === 'string' && (
                <WalletName>{name}</WalletName>
            )}
        </>
    );
};

WalletLabel.propTypes = {
    name: PropTypes.string,
};

export default WalletLabel;
